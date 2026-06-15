#include "TargetedCarUnlock.h"

#include "process/process.h"
#include "game/cdatabase.h"
#include "game/sql_inject.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <sstream>
#include <string>
#include <optional>
#include <unordered_map>
#include <vector>
#include <variant>
#include <cstdint>

namespace {
    bool RunSql(HANDLE process, const game::CDatabase& db, const std::string& sql, std::string& error) {
        auto result = game::execute_sql(process, db, sql);
        if (!result.success) {
            error = result.error.empty() ? "SQL execution failed" : result.error;
            return false;
        }
        return true;
    }

    std::string ForCarSql(const char* prefix, int carId, const char* suffix) {
        std::ostringstream sql;
        sql << prefix << carId << suffix;
        return sql.str();
    }

    std::optional<int64_t> QueryScalarInt(HANDLE process, const game::CDatabase& db, const std::string& sql, std::string& error) {
        auto result = game::execute_sql(process, db, sql);
        if (!result.success) {
            error = result.error.empty() ? "SQL query failed" : result.error;
            return std::nullopt;
        }

        if (!result.parsed || result.parsed->rows.empty() || result.parsed->rows[0].empty()) {
            return std::nullopt;
        }

        const auto& value = result.parsed->rows[0][0];
        if (std::holds_alternative<int64_t>(value)) {
            return std::get<int64_t>(value);
        }

        return std::nullopt;
    }

    struct SqlStep {
        std::string label;
        std::string sql;
    };

    enum class FovStorageUnit {
        Degrees,
        Radians
    };

    struct CachedFovTarget {
        uintptr_t address = 0;
        FovStorageUnit unit = FovStorageUnit::Degrees;
        float originalValue = 0.0f;
    };

    std::vector<CachedFovTarget> g_fovTargets;

    constexpr float kPi = 3.14159265358979323846f;
    constexpr size_t kFovMaxTargets = 48;

    bool IsReadableProtect(DWORD protect) {
        protect &= 0xff;
        return protect == PAGE_READONLY ||
               protect == PAGE_READWRITE ||
               protect == PAGE_WRITECOPY ||
               protect == PAGE_EXECUTE_READ ||
               protect == PAGE_EXECUTE_READWRITE ||
               protect == PAGE_EXECUTE_WRITECOPY;
    }

    bool IsWritableProtect(DWORD protect) {
        protect &= 0xff;
        return protect == PAGE_READWRITE ||
               protect == PAGE_WRITECOPY ||
               protect == PAGE_EXECUTE_READWRITE ||
               protect == PAGE_EXECUTE_WRITECOPY;
    }

    float ClampFov(float value) {
        if (!std::isfinite(value)) {
            return 90.0f;
        }
        return (std::clamp)(value, 1.0f, 200.0f);
    }

    float DegreesToRadians(float value) {
        return value * (kPi / 180.0f);
    }

    float RadiansToDegrees(float value) {
        return value * (180.0f / kPi);
    }

    bool NearlyEqual(float a, float b, float epsilon = 0.05f) {
        return std::fabs(a - b) <= epsilon;
    }

    bool LooksLikeDegreeFov(float value) {
        return std::isfinite(value) && value >= 45.0f && value <= 120.0f;
    }

    bool LooksLikeRadianFov(float value) {
        if (!std::isfinite(value) || value < 0.70f || value > 2.10f) {
            return false;
        }
        const float degrees = RadiansToDegrees(value);
        return degrees >= 45.0f && degrees <= 120.0f;
    }

    bool LooksLikeStoredFov(float value, FovStorageUnit unit) {
        return unit == FovStorageUnit::Degrees ? LooksLikeDegreeFov(value) : LooksLikeRadianFov(value);
    }

    float StoredFovToDegrees(float value, FovStorageUnit unit) {
        return unit == FovStorageUnit::Degrees ? value : RadiansToDegrees(value);
    }

    float TargetFovForUnit(float targetDegrees, FovStorageUnit unit) {
        return unit == FovStorageUnit::Degrees ? targetDegrees : DegreesToRadians(targetDegrees);
    }

    bool IsCommonCameraFov(float degrees) {
        static constexpr std::array<float, 9> common = {
            50.0f, 55.0f, 60.0f, 65.0f, 70.0f, 75.0f, 80.0f, 90.0f, 100.0f
        };

        for (float value : common) {
            if (std::fabs(degrees - value) <= 0.35f) {
                return true;
            }
        }
        return false;
    }

    int ScoreFovCandidate(const std::vector<uint8_t>& buffer, size_t offset, FovStorageUnit unit, int pageHits) {
        float value = 0.0f;
        std::memcpy(&value, buffer.data() + offset, sizeof(value));

        int score = 0;
        const float degrees = StoredFovToDegrees(value, unit);
        if (IsCommonCameraFov(degrees)) {
            score += 4;
        }

        if (pageHits >= 2 && pageHits <= 32) {
            score += 3;
        } else if (pageHits > 64) {
            score -= 6;
        }

        int fovLikeNeighbors = 0;
        int projectionLikeNeighbors = 0;
        const size_t begin = offset > 160 ? offset - 160 : 0;
        const size_t end = (std::min)(buffer.size() - sizeof(float), offset + 160);
        for (size_t cursor = begin; cursor <= end; cursor += sizeof(float)) {
            float nearby = 0.0f;
            std::memcpy(&nearby, buffer.data() + cursor, sizeof(nearby));
            if (!std::isfinite(nearby)) {
                continue;
            }

            if (LooksLikeDegreeFov(nearby) || LooksLikeRadianFov(nearby)) {
                ++fovLikeNeighbors;
            }
            if ((nearby >= 1.0f && nearby <= 2.5f) ||
                (nearby >= 0.001f && nearby <= 1.0f) ||
                (nearby >= 100.0f && nearby <= 20000.0f)) {
                ++projectionLikeNeighbors;
            }
        }

        if (fovLikeNeighbors >= 2) {
            score += 2;
        }
        if (projectionLikeNeighbors >= 3) {
            score += 1;
        }

        return score;
    }

    std::vector<CachedFovTarget> DiscoverFovTargets(HANDLE process, int& rawCandidateCount) {
        struct RawCandidate {
            uintptr_t address = 0;
            size_t offset = 0;
            FovStorageUnit unit = FovStorageUnit::Degrees;
            float originalValue = 0.0f;
        };

        struct ScoredCandidate {
            RawCandidate candidate;
            int score = 0;
        };

        rawCandidateCount = 0;
        std::vector<ScoredCandidate> accepted;
        SYSTEM_INFO sys{};
        GetSystemInfo(&sys);

        uintptr_t address = reinterpret_cast<uintptr_t>(sys.lpMinimumApplicationAddress);
        const uintptr_t maxAddress = reinterpret_cast<uintptr_t>(sys.lpMaximumApplicationAddress);
        constexpr size_t kChunkSize = 2 * 1024 * 1024;

        while (address < maxAddress && accepted.size() < kFovMaxTargets) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi))) {
                address += 0x10000;
                continue;
            }

            const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            const uintptr_t regionEnd = regionBase + mbi.RegionSize;
            address = regionEnd;

            if (mbi.State != MEM_COMMIT ||
                (mbi.Protect & PAGE_NOACCESS) ||
                (mbi.Protect & PAGE_GUARD) ||
                !IsReadableProtect(mbi.Protect) ||
                !IsWritableProtect(mbi.Protect)) {
                continue;
            }

            if (mbi.Type != MEM_PRIVATE && mbi.Type != MEM_MAPPED && mbi.Type != MEM_IMAGE) {
                continue;
            }

            for (uintptr_t chunk = regionBase; chunk < regionEnd && accepted.size() < kFovMaxTargets; chunk += kChunkSize) {
                const uintptr_t remaining = regionEnd - chunk;
                const size_t requested = static_cast<size_t>((std::min)(static_cast<uintptr_t>(kChunkSize), remaining));
                if (requested < sizeof(float)) {
                    continue;
                }

                std::vector<uint8_t> buffer(requested);
                SIZE_T bytesRead = 0;
                if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(chunk), buffer.data(), buffer.size(), &bytesRead) ||
                    bytesRead < sizeof(float)) {
                    continue;
                }
                buffer.resize(static_cast<size_t>(bytesRead));

                std::vector<RawCandidate> raw;
                std::unordered_map<uintptr_t, int> pageHits;
                const size_t limit = buffer.size() - sizeof(float);
                for (size_t offset = 0; offset <= limit; offset += sizeof(float)) {
                    float value = 0.0f;
                    std::memcpy(&value, buffer.data() + offset, sizeof(value));

                    FovStorageUnit unit = FovStorageUnit::Degrees;
                    if (LooksLikeDegreeFov(value)) {
                        unit = FovStorageUnit::Degrees;
                    } else if (LooksLikeRadianFov(value)) {
                        unit = FovStorageUnit::Radians;
                    } else {
                        continue;
                    }

                    ++rawCandidateCount;
                    const uintptr_t hitAddress = chunk + offset;
                    ++pageHits[hitAddress & ~static_cast<uintptr_t>(0xfff)];
                    raw.push_back({ hitAddress, offset, unit, value });
                }

                std::vector<ScoredCandidate> scored;
                scored.reserve(raw.size());
                for (const auto& candidate : raw) {
                    const uintptr_t page = candidate.address & ~static_cast<uintptr_t>(0xfff);
                    const int hitsOnPage = pageHits[page];
                    const int score = ScoreFovCandidate(buffer, candidate.offset, candidate.unit, hitsOnPage);
                    if (score >= 4) {
                        scored.push_back({ candidate, score });
                    }
                }

                std::sort(scored.begin(), scored.end(), [](const ScoredCandidate& lhs, const ScoredCandidate& rhs) {
                    return lhs.score > rhs.score;
                });

                for (const auto& item : scored) {
                    const auto existing = std::find_if(accepted.begin(), accepted.end(), [&](const ScoredCandidate& other) {
                        return other.candidate.address == item.candidate.address;
                    });
                    if (existing != accepted.end()) {
                        continue;
                    }

                    accepted.push_back(item);
                    if (accepted.size() >= kFovMaxTargets) {
                        break;
                    }
                }
            }
        }

        std::sort(accepted.begin(), accepted.end(), [](const ScoredCandidate& lhs, const ScoredCandidate& rhs) {
            if (lhs.score != rhs.score) {
                return lhs.score > rhs.score;
            }
            return lhs.candidate.address < rhs.candidate.address;
        });

        std::vector<CachedFovTarget> targets;
        targets.reserve((std::min)(accepted.size(), kFovMaxTargets));
        for (const auto& item : accepted) {
            targets.push_back({
                item.candidate.address,
                item.candidate.unit,
                item.candidate.originalValue
            });
        }
        return targets;
    }

    FovOverrideResult WriteFovTargets(HANDLE process, float targetFov) {
        FovOverrideResult result;
        result.candidates = static_cast<int>(g_fovTargets.size());

        const float clampedTarget = ClampFov(targetFov);
        for (const auto& target : g_fovTargets) {
            auto current = process::read_value<float>(process, target.address);
            if (!current || !std::isfinite(*current)) {
                continue;
            }

            const float desired = TargetFovForUnit(clampedTarget, target.unit);
            const bool alreadyTarget = NearlyEqual(*current, desired, target.unit == FovStorageUnit::Degrees ? 0.10f : 0.003f);
            const bool stillLooksValid =
                alreadyTarget ||
                LooksLikeStoredFov(*current, target.unit) ||
                NearlyEqual(*current, target.originalValue, target.unit == FovStorageUnit::Degrees ? 0.50f : 0.01f);

            if (!stillLooksValid) {
                continue;
            }

            if (process::write_memory(process, target.address, &desired, sizeof(desired))) {
                ++result.written;

                auto verify = process::read_value<float>(process, target.address);
                if (verify && NearlyEqual(*verify, desired, target.unit == FovStorageUnit::Degrees ? 0.10f : 0.003f)) {
                    ++result.verified;
                }
            }
        }

        result.success = result.verified > 0;
        if (result.success) {
            result.message = "FOV set to " + std::to_string(static_cast<int>(std::lround(clampedTarget))) +
                " (" + std::to_string(result.verified) + " live values verified).";
        } else if (result.candidates > 0) {
            result.message = "FOV targets were found, but none could be verified after writing.";
        } else {
            result.message = "No safe live FOV targets were found. Load into gameplay, switch camera once, then try again.";
        }

        return result;
    }

    MiscDbModResult ExecuteMiscDbMod(const std::vector<SqlStep>& steps, const std::string& successMessage) {
        MiscDbModResult outcome;

        auto proc = process::open_process(L"forzahorizon6.exe");
        if (!proc || !proc->handle || proc->base_address == 0 || proc->image_size == 0) {
            outcome.message = "Start FH6 first, then run this tool as administrator.";
            return outcome;
        }

        auto db = game::resolve_cdatabase(proc->handle, proc->base_address, proc->image_size);
        if (!db) {
            process::close_process(*proc);
            outcome.message = "Could not find FH6 CDatabase. This game build may need a new signature.";
            return outcome;
        }

        std::string error;
        for (const auto& step : steps) {
            if (RunSql(proc->handle, *db, step.sql, error)) {
                ++outcome.succeeded;
                continue;
            }

            ++outcome.failed;
            process::close_process(*proc);
            outcome.message = step.label + " failed: " + (error.empty() ? "SQL execution failed" : error);
            return outcome;
        }

        process::close_process(*proc);
        outcome.success = true;
        outcome.message = successMessage + " (" + std::to_string(outcome.succeeded) + " SQL steps OK).";
        return outcome;
    }

    bool PrepareCarForGarage(HANDLE process, const game::CDatabase& db, int carId, std::string& error) {
        std::vector<SqlStep> steps;

        steps.push_back({
            "Backup car visibility",
            "CREATE TABLE IF NOT EXISTS _fh6_tool_CarVisibilityBackup AS "
            "SELECT Id, IsDrivable, NotAvailableInAutoshow, VisibleOnlyIfOwned FROM Data_Car WHERE 0"
        });

        {
            std::ostringstream sql;
            sql << R"sql(
INSERT INTO _fh6_tool_CarVisibilityBackup(Id, IsDrivable, NotAvailableInAutoshow, VisibleOnlyIfOwned)
SELECT Id, IsDrivable, NotAvailableInAutoshow, VisibleOnlyIfOwned
FROM Data_Car
WHERE Id = )sql" << carId << R"sql(
  AND NOT EXISTS (SELECT 1 FROM _fh6_tool_CarVisibilityBackup WHERE Id = )sql" << carId << R"sql()
)sql";
            steps.push_back({ "Store original car visibility", sql.str() });
        }

        steps.push_back({
            "Make car garage-visible",
            ForCarSql("UPDATE Data_Car SET IsDrivable = 1, NotAvailableInAutoshow = 0, VisibleOnlyIfOwned = 0 WHERE Id = ", carId, "")
        });

        steps.push_back({
            "Backup unobtainable state",
            "CREATE TABLE IF NOT EXISTS _fh6_tool_UnobtainableCarsBackup AS "
            "SELECT Ordinal FROM UnobtainableCars WHERE 0"
        });

        {
            std::ostringstream sql;
            sql << "INSERT INTO _fh6_tool_UnobtainableCarsBackup(Ordinal)\n"
                << "SELECT Ordinal\n"
                << "FROM UnobtainableCars\n"
                << "WHERE Ordinal IN (" << carId
                << ", COALESCE((SELECT ModelId FROM Data_Car WHERE Id = " << carId << "), " << carId << "))\n"
                << "  AND NOT EXISTS (SELECT 1 FROM _fh6_tool_UnobtainableCarsBackup b WHERE b.Ordinal = UnobtainableCars.Ordinal)";
            steps.push_back({ "Store original unobtainable state", sql.str() });
        }

        {
            std::ostringstream sql;
            sql << "DELETE FROM UnobtainableCars WHERE Ordinal IN (" << carId
                << ", COALESCE((SELECT ModelId FROM Data_Car WHERE Id = " << carId << "), " << carId << "))";
            steps.push_back({ "Remove unobtainable block", sql.str() });
        }

        steps.push_back({
            "Backup car bucket",
            "CREATE TABLE IF NOT EXISTS _fh6_tool_CarBucketBackup AS "
            "SELECT CarId, CarBucket, BucketHero FROM Data_Car_Buckets WHERE 0"
        });

        {
            std::ostringstream sql;
            sql << R"sql(
INSERT INTO _fh6_tool_CarBucketBackup(CarId, CarBucket, BucketHero)
SELECT CarId, CarBucket, BucketHero
FROM Data_Car_Buckets
WHERE CarId = )sql" << carId << R"sql(
  AND NOT EXISTS (SELECT 1 FROM _fh6_tool_CarBucketBackup WHERE CarId = )sql" << carId << R"sql()
)sql";
            steps.push_back({ "Store original car bucket", sql.str() });
        }

        {
            std::ostringstream sql;
            sql << R"sql(
INSERT INTO Data_Car_Buckets(CarId, CarBucket, BucketHero)
SELECT d.Id,
       COALESCE((SELECT CarBucket FROM Data_Car_Buckets WHERE CarBucket IS NOT NULL GROUP BY CarBucket ORDER BY COUNT(*) DESC LIMIT 1), 1),
       0
FROM Data_Car d
WHERE d.Id = )sql" << carId << R"sql(
  AND NOT EXISTS (SELECT 1 FROM Data_Car_Buckets WHERE CarId = d.Id)
)sql";
            steps.push_back({ "Add car bucket", sql.str() });
        }

        {
            std::ostringstream sql;
            sql << R"sql(
INSERT OR IGNORE INTO Data_Car_Buckets(CarId, CarBucket, BucketHero)
SELECT d.ModelId,
       COALESCE((SELECT CarBucket FROM Data_Car_Buckets WHERE CarBucket IS NOT NULL GROUP BY CarBucket ORDER BY COUNT(*) DESC LIMIT 1), 1),
       0
FROM Data_Car d
WHERE d.Id = )sql" << carId << R"sql(
  AND d.ModelId IS NOT NULL
)sql";
            steps.push_back({ "Add model bucket", sql.str() });
        }

        {
            std::ostringstream sql;
            sql << "UPDATE Data_Car_Buckets\n"
                << "SET CarBucket = COALESCE(CarBucket, (SELECT CarBucket FROM Data_Car_Buckets WHERE CarBucket IS NOT NULL GROUP BY CarBucket ORDER BY COUNT(*) DESC LIMIT 1), 1),\n"
                << "    BucketHero = COALESCE(BucketHero, 0)\n"
                << "WHERE CarId IN (" << carId
                << ", COALESCE((SELECT ModelId FROM Data_Car WHERE Id = " << carId << "), " << carId << "))\n"
                << "  AND (CarBucket IS NULL OR BucketHero IS NULL)";
            steps.push_back({ "Normalize car bucket", sql.str() });
        }

        steps.push_back({ "Drop Drivable_Data_Car view", "DROP VIEW IF EXISTS Drivable_Data_Car" });
        steps.push_back({ "Recreate Drivable_Data_Car view", "CREATE VIEW Drivable_Data_Car AS SELECT Data_Car.* FROM Data_Car WHERE Id NOT IN (SELECT Ordinal FROM UnobtainableCars)" });

        for (const auto& step : steps) {
            if (RunSql(process, db, step.sql, error)) {
                continue;
            }

            error = step.label + " failed: " + (error.empty() ? "SQL execution failed" : error);
            return false;
        }

        return true;
    }

    std::string BuildGrantGarageSql(int carId, bool allowDuplicate, std::optional<int> fixedGarageId = std::nullopt) {
        std::ostringstream sql;
        sql << R"sql(
INSERT INTO Profile0_Career_Garage (
    Id, CarId, PerformanceIndex, ClassID, PartsValue, SpeedRating, OffroadRating,
    AccelerationRating, LaunchRating, BrakingRating, HandlingRating, CurbWeight,
    WeightDistribution, AspirationTypeId, SimPeakPower, SimPeakAngVel,
    SimPeakTorque, SimPeakTorqueAngVel, SimRedlineAngVel, PeakIntakePSI,
    TopSpeed, DistanceDriven, TimeDriven, TotalWinnings, TotalRepairs,
    NumVictories, NumPodiums, NumRaces, NumOwners, NumTimesSold,
    TimeDrivenInRoadTrips, CurOwnerNumRaces, CurOwnerWinnings,
    NumSkillPointsEarned, HighestSkillScore, OriginalOwner, CarGroup, Flags,
    Engine, Drivetrain, CarBody, Motor, Brakes, SpringDamper, AntiSwayFront,
    AntiSwayRear, TireCompound, RearWing, RimSizeFront, RimSizeRear, Camshaft,
    Valves, Displacement, PistonsCompression, FuelSystem, Ignition, Exhaust,
    Intake, Flywheel, Manifold, RestrictorPlate, OilCooling, SingleTurbo,
    TwinTurbo, QuadTurbo, SuperchargerCSC, SuperchargerDSC, Intercooler,
    Clutch, Transmission, Driveline, Differential, FrontBumper, RearBumper,
    Hood, SideSkirts, TireWidthFront, TireWidthRear, WeightReduction,
    ChassisStiffness, TrackSpacingFront, TrackSpacingRear, FrontAspectRatio,
    RearAspectRatio, MotorParts, TireBrand, WheelStyle, WheelStyleRear,
    DefaultManufacturerColorIndex, LiveryFileName, TuneFileName,
    VersionedTuneId, VersionedTuneXUID, VersionedLiveryId, Guid, Thumbnail,
    Tuning_frontTirePressure, Tuning_rearTirePressure, Tuning_finalDriveRatio,
    Tuning_firstGear, Tuning_secondGear, Tuning_thirdGear, Tuning_fourthGear,
    Tuning_fifthGear, Tuning_sixthGear, Tuning_seventhGear, Tuning_eighthGear,
    Tuning_ninthGear, Tuning_tenthGear, Tuning_frontCamber, Tuning_rearCamber,
    Tuning_frontToe, Tuning_rearToe, Tuning_frontCaster, Tuning_frontSwaybar,
    Tuning_rearSwaybar, Tuning_frontSpring, Tuning_rearSpring,
    Tuning_frontRideHeight, Tuning_rearRideHeight,
    Tuning_frontDampingStiffness, Tuning_rearDampingStiffness,
    Tuning_frontBumpRatio, Tuning_rearBumpRatio, Tuning_frontDownforce,
    Tuning_rearDownforce, Tuning_brakeBalance, Tuning_brakePressure,
    Tuning_frontAccel, Tuning_rearAccel, Tuning_frontDecel, Tuning_rearDecel,
    Tuning_centerTorque, RebuildModTorque, RebuildModGrip, RebuildModBraking,
    RebuildModWeight, RebuildScore, SharedID, IsFavorite,
    HasCurrentOwnerViewedCar, Traction_Road, Traction_OffRoad, Traction_Snow,
    FrontTireAspectRatioOffset, RearTireAspectRatioOffset
)
SELECT
)sql";
        if (fixedGarageId && *fixedGarageId > 0) {
            sql << *fixedGarageId;
        } else {
            sql << "COALESCE((SELECT MAX(Id) + 1 FROM Profile0_Career_Garage), 1)";
        }
        sql << R"sql(,
    d.Id, d.PerformanceIndex, d.ClassID, 0, d.SpeedRating, d.OffroadRating,
    d.AccelerationRating, d.LaunchRating, d.BrakingRating, d.HandlingRating,
    d.CurbWeight, d.WeightDistribution, d.AspirationTypeId, d.SimPeakPower,
    d.SimPeakAngVel, d.SimPeakTorque, d.SimPeakTorqueAngVel, d.SimRedlineAngVel,
    0.0, 0.0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
    COALESCE((SELECT OriginalOwner FROM Profile0_Career_Garage WHERE OriginalOwner IS NOT NULL AND OriginalOwner <> '' LIMIT 1), ''),
    NULL, 8,
    COALESCE((SELECT Id FROM List_UpgradeEngine WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeDrivetrain WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeCarBody WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeMotor WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeBrakes WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeSpringDamper WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeAntiSwayFront WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeAntiSwayRear WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeTireCompound WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeRearWing WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeRimSizeFront WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeRimSizeRear WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeEngineCamshaft WHERE EngineID = (SELECT EngineID FROM List_UpgradeEngine WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeEngineValves WHERE EngineID = (SELECT EngineID FROM List_UpgradeEngine WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeEngineDisplacement WHERE EngineID = (SELECT EngineID FROM List_UpgradeEngine WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeEnginePistonsCompression WHERE EngineID = (SELECT EngineID FROM List_UpgradeEngine WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeEngineFuelSystem WHERE EngineID = (SELECT EngineID FROM List_UpgradeEngine WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeEngineIgnition WHERE EngineID = (SELECT EngineID FROM List_UpgradeEngine WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeEngineExhaust WHERE EngineID = (SELECT EngineID FROM List_UpgradeEngine WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeEngineIntake WHERE EngineID = (SELECT EngineID FROM List_UpgradeEngine WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeEngineFlywheel WHERE EngineID = (SELECT EngineID FROM List_UpgradeEngine WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeEngineManifold WHERE EngineID = (SELECT EngineID FROM List_UpgradeEngine WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeEngineRestrictorPlate WHERE EngineID = (SELECT EngineID FROM List_UpgradeEngine WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeEngineOilCooling WHERE EngineID = (SELECT EngineID FROM List_UpgradeEngine WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeEngineTurboSingle WHERE EngineID = (SELECT EngineID FROM List_UpgradeEngine WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeEngineTurboTwin WHERE EngineID = (SELECT EngineID FROM List_UpgradeEngine WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeEngineTurboQuad WHERE EngineID = (SELECT EngineID FROM List_UpgradeEngine WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeEngineCSC WHERE EngineID = (SELECT EngineID FROM List_UpgradeEngine WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeEngineDSC WHERE EngineID = (SELECT EngineID FROM List_UpgradeEngine WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeEngineIntercooler WHERE EngineID = (SELECT EngineID FROM List_UpgradeEngine WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeDrivetrainClutch WHERE DrivetrainID = (SELECT DrivetrainID FROM List_UpgradeDrivetrain WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeDrivetrainTransmission WHERE DrivetrainID = (SELECT DrivetrainID FROM List_UpgradeDrivetrain WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeDrivetrainDriveline WHERE DrivetrainID = (SELECT DrivetrainID FROM List_UpgradeDrivetrain WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeDrivetrainDifferential WHERE DrivetrainID = (SELECT DrivetrainID FROM List_UpgradeDrivetrain WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeCarBodyFrontBumper WHERE CarBodyID = (SELECT CarBodyID FROM List_UpgradeCarBody WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeCarBodyRearBumper WHERE CarBodyID = (SELECT CarBodyID FROM List_UpgradeCarBody WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeCarBodyHood WHERE CarBodyID = (SELECT CarBodyID FROM List_UpgradeCarBody WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeCarBodySideSkirt WHERE CarBodyID = (SELECT CarBodyID FROM List_UpgradeCarBody WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeCarBodyTireWidthFront WHERE CarBodyId = (SELECT CarBodyID FROM List_UpgradeCarBody WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeCarBodyTireWidthRear WHERE CarBodyId = (SELECT CarBodyID FROM List_UpgradeCarBody WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeCarBodyWeight WHERE CarBodyId = (SELECT CarBodyID FROM List_UpgradeCarBody WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeCarBodyChassisStiffness WHERE CarbodyId = (SELECT CarBodyID FROM List_UpgradeCarBody WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND Level = 0 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeCarBodyTrackSpacingFront WHERE CarBodyId = (SELECT CarBodyID FROM List_UpgradeCarBody WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeCarBodyTrackSpacingRear WHERE CarBodyId = (SELECT CarBodyID FROM List_UpgradeCarBody WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeCarBodyTireAspectRatioFront WHERE CarBodyId = (SELECT CarBodyID FROM List_UpgradeCarBody WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeCarBodyTireAspectRatioRear WHERE CarBodyId = (SELECT CarBodyID FROM List_UpgradeCarBody WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    COALESCE((SELECT Id FROM List_UpgradeMotorParts WHERE MotorID = (SELECT MotorID FROM List_UpgradeMotor WHERE Ordinal = d.Id AND IsStock = 1 LIMIT 1) AND IsStock = 1 LIMIT 1), -1),
    NULL, -1, -1, 0, '', '', '00000000-0000-0000-0000-000000000000', NULL,
    '00000000-0000-0000-0000-000000000000',
    (SELECT lower(substr(x,1,8)||'-'||substr(x,9,4)||'-4'||substr(x,14,3)||'-'||substr('89ab', abs(random()) % 4 + 1, 1)||substr(x,18,3)||'-'||substr(x,21,12)) FROM (SELECT hex(randomblob(16)) AS x)),
    d.Thumbnail,
    -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0,
    -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0,
    -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, NULL, NULL, NULL, NULL, NULL, 0, NULL, 0,
    d.Traction_Road, d.Traction_OffRoad, d.Traction_Snow, -1, -1
FROM Data_Car d
WHERE d.Id = )sql" << carId;
        if (!allowDuplicate && (!fixedGarageId || *fixedGarageId <= 0)) {
            sql << "\n  AND NOT EXISTS (SELECT 1 FROM Profile0_Career_Garage WHERE CarId = d.Id)";
        }
        sql << "\n";
        return sql.str();
    }
}

TargetedCarUnlockResult GrantCarToGarage(int carId, bool allowDuplicate, bool prepareForGarage) {
    TargetedCarUnlockResult outcome;

    auto proc = process::open_process(L"forzahorizon6.exe");
    if (!proc || !proc->handle || proc->base_address == 0 || proc->image_size == 0) {
        outcome.message = "Start FH6 first, then run this tool as administrator.";
        return outcome;
    }

    auto db = game::resolve_cdatabase(proc->handle, proc->base_address, proc->image_size);
    if (!db) {
        process::close_process(*proc);
        outcome.message = "Could not find FH6 CDatabase. This game build may need a new signature.";
        return outcome;
    }

    std::string error;
    if (prepareForGarage && !PrepareCarForGarage(proc->handle, *db, carId, error)) {
        process::close_process(*proc);
        outcome.message = error.empty() ? "Could not prepare this car for garage visibility." : error;
        return outcome;
    }

    auto beforeCount = QueryScalarInt(proc->handle, *db,
        ForCarSql("SELECT COUNT(*) FROM Profile0_Career_Garage WHERE CarId = ", carId, ""), error);
    if (!beforeCount) {
        process::close_process(*proc);
        outcome.message = error.empty() ? "Could not check current garage." : error;
        return outcome;
    }

    outcome.ownedCount = (int)*beforeCount;

    if (*beforeCount > 0 && !allowDuplicate) {
        process::close_process(*proc);
        outcome.alreadyOwned = true;
        outcome.message = prepareForGarage
            ? "This car is already in the garage database; visibility prep was refreshed."
            : "You already have this car in your garage.";
        return outcome;
    }

    if (!RunSql(proc->handle, *db, BuildGrantGarageSql(carId, allowDuplicate), error)) {
        process::close_process(*proc);
        outcome.message = error;
        return outcome;
    }

    auto afterCount = QueryScalarInt(proc->handle, *db,
        ForCarSql("SELECT COUNT(*) FROM Profile0_Career_Garage WHERE CarId = ", carId, ""), error);
    if (!afterCount || *afterCount <= *beforeCount) {
        process::close_process(*proc);
        outcome.message = allowDuplicate
            ? "Garage insert did not add a duplicate row for this car."
            : "Garage insert did not add a row for this car.";
        return outcome;
    }

    auto garageId = QueryScalarInt(proc->handle, *db,
        ForCarSql("SELECT MAX(Id) FROM Profile0_Career_Garage WHERE CarId = ", carId, ""), error);
    if (garageId) {
        outcome.garageId = (int)*garageId;
    }

    process::close_process(*proc);
    outcome.success = true;
    outcome.ownedCount = (int)*afterCount;
    if (prepareForGarage) {
        outcome.message = allowDuplicate
            ? "Prepared duplicate car was added to your garage database."
            : "Prepared car was added to your garage database.";
    } else {
        outcome.message = allowDuplicate
            ? "Duplicate car was added to your garage."
            : "Selected car was added to your garage.";
    }
    return outcome;
}

TargetedCarUnlockResult SetTemporaryLeadPlayerCar(int carId) {
    TargetedCarUnlockResult outcome;

    if (carId <= 0) {
        outcome.message = "Invalid car ID.";
        return outcome;
    }

    auto proc = process::open_process(L"forzahorizon6.exe");
    if (!proc || !proc->handle || proc->base_address == 0 || proc->image_size == 0) {
        outcome.message = "Start FH6 first, then run this tool as administrator.";
        return outcome;
    }

    auto db = game::resolve_cdatabase(proc->handle, proc->base_address, proc->image_size);
    if (!db) {
        process::close_process(*proc);
        outcome.message = "Could not find FH6 CDatabase. This game build may need a new signature.";
        return outcome;
    }

    std::string error;
    if (!PrepareCarForGarage(proc->handle, *db, carId, error)) {
        process::close_process(*proc);
        outcome.message = error.empty() ? "Could not prepare this car for temporary use." : error;
        return outcome;
    }

    std::vector<SqlStep> steps;
    steps.push_back({
        "Drop previous transient player car",
        "DROP TABLE IF EXISTS tmpLeadPlayerCar"
    });

    {
        std::ostringstream sql;
        sql << R"sql(
CREATE TABLE tmpLeadPlayerCar AS
SELECT *, 0 AS InstalledPartsTireCompoundId
FROM Data_Car
WHERE Id = )sql" << carId;
        steps.push_back({ "Create transient player car", sql.str() });
    }

    {
        std::ostringstream sql;
        sql << R"sql(
UPDATE tmpLeadPlayerCar
SET InstalledPartsTireCompoundId = COALESCE((
    SELECT Id FROM List_UpgradeTireCompound
    WHERE Ordinal = )sql" << carId << R"sql(
      AND IsStock = 1
    LIMIT 1
), 0)
)sql";
        steps.push_back({ "Set transient tire compound", sql.str() });
    }

    for (const auto& step : steps) {
        if (RunSql(proc->handle, *db, step.sql, error)) {
            continue;
        }

        process::close_process(*proc);
        outcome.message = step.label + " failed: " + (error.empty() ? "SQL execution failed" : error);
        return outcome;
    }

    auto rowCount = QueryScalarInt(proc->handle, *db,
        "SELECT COUNT(*) FROM tmpLeadPlayerCar", error);
    if (!rowCount || *rowCount <= 0) {
        process::close_process(*proc);
        outcome.message = "Transient player car table did not verify.";
        return outcome;
    }

    process::close_process(*proc);
    outcome.success = true;
    outcome.ownedCount = 1;
    outcome.message = "Temporary lead player car was set.";
    return outcome;
}

TargetedCarUnlockResult BorrowCarOverGarageRow(int carId, int garageId, bool prepareForGarage) {
    TargetedCarUnlockResult outcome;
    outcome.garageId = garageId;

    if (carId <= 0 || garageId <= 0) {
        outcome.message = "Invalid car or garage row ID.";
        return outcome;
    }

    auto proc = process::open_process(L"forzahorizon6.exe");
    if (!proc || !proc->handle || proc->base_address == 0 || proc->image_size == 0) {
        outcome.message = "Start FH6 first, then run this tool as administrator.";
        return outcome;
    }

    auto db = game::resolve_cdatabase(proc->handle, proc->base_address, proc->image_size);
    if (!db) {
        process::close_process(*proc);
        outcome.message = "Could not find FH6 CDatabase. This game build may need a new signature.";
        return outcome;
    }

    std::string error;
    auto existingCarId = QueryScalarInt(proc->handle, *db,
        ForCarSql("SELECT CarId FROM Profile0_Career_Garage WHERE Id = ", garageId, " LIMIT 1"), error);
    if (!existingCarId) {
        process::close_process(*proc);
        outcome.message = error.empty() ? "Selected garage row was not found." : error;
        return outcome;
    }
    outcome.replacedCarId = (int)*existingCarId;

    if (prepareForGarage && !PrepareCarForGarage(proc->handle, *db, carId, error)) {
        process::close_process(*proc);
        outcome.message = error.empty() ? "Could not prepare this car for garage visibility." : error;
        return outcome;
    }

    if (!RunSql(proc->handle, *db,
            "CREATE TABLE IF NOT EXISTS _fh6_tool_BorrowedGarageRows AS "
            "SELECT * FROM Profile0_Career_Garage WHERE 0", error)) {
        process::close_process(*proc);
        outcome.message = error.empty() ? "Could not create borrow backup table." : error;
        return outcome;
    }

    {
        std::ostringstream backupSql;
        backupSql << "INSERT INTO _fh6_tool_BorrowedGarageRows "
                  << "SELECT * FROM Profile0_Career_Garage WHERE Id = " << garageId
                  << " AND NOT EXISTS (SELECT 1 FROM _fh6_tool_BorrowedGarageRows WHERE Id = " << garageId << ")";
        if (!RunSql(proc->handle, *db, backupSql.str(), error)) {
            process::close_process(*proc);
            outcome.message = error.empty() ? "Could not back up the selected garage row." : error;
            return outcome;
        }
    }

    if (!RunSql(proc->handle, *db, ForCarSql("DELETE FROM Profile0_Career_Garage WHERE Id = ", garageId, ""), error)) {
        process::close_process(*proc);
        outcome.message = error.empty() ? "Could not clear the selected garage row." : error;
        return outcome;
    }

    if (!RunSql(proc->handle, *db, BuildGrantGarageSql(carId, true, garageId), error)) {
        std::string restoreError;
        RunSql(proc->handle, *db,
            ForCarSql("INSERT INTO Profile0_Career_Garage SELECT * FROM _fh6_tool_BorrowedGarageRows WHERE Id = ", garageId, ""),
            restoreError);
        process::close_process(*proc);
        outcome.message = (error.empty() ? "Borrow insert failed." : error) +
            (restoreError.empty() ? "" : " Restore also failed: " + restoreError);
        return outcome;
    }

    auto finalCarId = QueryScalarInt(proc->handle, *db,
        ForCarSql("SELECT CarId FROM Profile0_Career_Garage WHERE Id = ", garageId, " LIMIT 1"), error);
    if (!finalCarId || *finalCarId != carId) {
        process::close_process(*proc);
        outcome.message = "Borrow row replacement did not verify.";
        return outcome;
    }

    process::close_process(*proc);
    outcome.success = true;
    outcome.message = "Selected garage row was replaced with the borrowed car.";
    return outcome;
}

TargetedCarUnlockResult RestoreBorrowedGarageRow(int garageId) {
    TargetedCarUnlockResult outcome;
    outcome.garageId = garageId;

    if (garageId <= 0) {
        outcome.message = "Invalid garage row ID.";
        return outcome;
    }

    auto proc = process::open_process(L"forzahorizon6.exe");
    if (!proc || !proc->handle || proc->base_address == 0 || proc->image_size == 0) {
        outcome.message = "Start FH6 first, then run this tool as administrator.";
        return outcome;
    }

    auto db = game::resolve_cdatabase(proc->handle, proc->base_address, proc->image_size);
    if (!db) {
        process::close_process(*proc);
        outcome.message = "Could not find FH6 CDatabase. This game build may need a new signature.";
        return outcome;
    }

    std::string error;
    if (!RunSql(proc->handle, *db,
            "CREATE TABLE IF NOT EXISTS _fh6_tool_BorrowedGarageRows AS "
            "SELECT * FROM Profile0_Career_Garage WHERE 0", error)) {
        process::close_process(*proc);
        outcome.message = error.empty() ? "Could not open borrow backup table." : error;
        return outcome;
    }

    auto backupCarId = QueryScalarInt(proc->handle, *db,
        ForCarSql("SELECT CarId FROM _fh6_tool_BorrowedGarageRows WHERE Id = ", garageId, " LIMIT 1"), error);
    if (!backupCarId) {
        process::close_process(*proc);
        outcome.message = error.empty() ? "No backup exists for that garage row." : error;
        return outcome;
    }

    if (!RunSql(proc->handle, *db, ForCarSql("DELETE FROM Profile0_Career_Garage WHERE Id = ", garageId, ""), error)) {
        process::close_process(*proc);
        outcome.message = error.empty() ? "Could not clear borrowed row before restore." : error;
        return outcome;
    }

    if (!RunSql(proc->handle, *db,
            ForCarSql("INSERT INTO Profile0_Career_Garage SELECT * FROM _fh6_tool_BorrowedGarageRows WHERE Id = ", garageId, ""),
            error)) {
        process::close_process(*proc);
        outcome.message = error.empty() ? "Could not restore borrowed row." : error;
        return outcome;
    }

    RunSql(proc->handle, *db, ForCarSql("DELETE FROM _fh6_tool_BorrowedGarageRows WHERE Id = ", garageId, ""), error);

    process::close_process(*proc);
    outcome.success = true;
    outcome.replacedCarId = (int)*backupCarId;
    outcome.message = "Borrowed garage row was restored.";
    return outcome;
}

TargetedCarUnlockResult RemoveGarageRowById(int garageId) {
    TargetedCarUnlockResult outcome;
    outcome.garageId = garageId;

    if (garageId <= 0) {
        outcome.message = "Invalid garage row ID.";
        return outcome;
    }

    auto proc = process::open_process(L"forzahorizon6.exe");
    if (!proc || !proc->handle || proc->base_address == 0 || proc->image_size == 0) {
        outcome.message = "Start FH6 first, then run this tool as administrator.";
        return outcome;
    }

    auto db = game::resolve_cdatabase(proc->handle, proc->base_address, proc->image_size);
    if (!db) {
        process::close_process(*proc);
        outcome.message = "Could not find FH6 CDatabase. This game build may need a new signature.";
        return outcome;
    }

    std::string error;
    auto existingCarId = QueryScalarInt(proc->handle, *db,
        ForCarSql("SELECT CarId FROM Profile0_Career_Garage WHERE Id = ", garageId, " LIMIT 1"), error);
    if (!existingCarId) {
        process::close_process(*proc);
        if (!error.empty()) {
            outcome.message = error;
            return outcome;
        }

        outcome.success = true;
        outcome.message = "Garage row was already gone.";
        return outcome;
    }

    std::ostringstream deleteSql;
    deleteSql << "DELETE FROM Profile0_Career_Garage WHERE Id = " << garageId;
    if (!RunSql(proc->handle, *db, deleteSql.str(), error)) {
        process::close_process(*proc);
        outcome.message = error.empty() ? "SQL DELETE failed." : error;
        return outcome;
    }

    error.clear();
    auto afterCount = QueryScalarInt(proc->handle, *db,
        ForCarSql("SELECT COUNT(*) FROM Profile0_Career_Garage WHERE Id = ", garageId, ""), error);
    if (afterCount && *afterCount > 0) {
        process::close_process(*proc);
        outcome.message = "Garage row delete did not remove the row.";
        return outcome;
    }

    process::close_process(*proc);
    outcome.success = true;
    outcome.message = "Garage row removed.";
    return outcome;
}

std::vector<GarageCarInfo> GetGarageCarIds(std::string& outError) {
    std::vector<GarageCarInfo> result;

    auto proc = process::open_process(L"forzahorizon6.exe");
    if (!proc || !proc->handle || proc->base_address == 0 || proc->image_size == 0) {
        outError = "Start FH6 first, then run this tool as administrator.";
        return result;
    }

    auto db = game::resolve_cdatabase(proc->handle, proc->base_address, proc->image_size);
    if (!db) {
        process::close_process(*proc);
        outError = "Could not find FH6 CDatabase.";
        return result;
    }

    auto sqlResult = game::execute_sql(proc->handle, *db,
        "SELECT CarId, Id FROM Profile0_Career_Garage ORDER BY Id");

    process::close_process(*proc);

    if (!sqlResult.success) {
        outError = sqlResult.error.empty() ? "SQL query failed." : sqlResult.error;
        return result;
    }

    if (sqlResult.parsed) {
        for (const auto& row : sqlResult.parsed->rows) {
            if (row.size() >= 2) {
                GarageCarInfo info;
                if (std::holds_alternative<int64_t>(row[0]))
                    info.carId = (int)std::get<int64_t>(row[0]);
                if (std::holds_alternative<int64_t>(row[1]))
                    info.garageId = (int)std::get<int64_t>(row[1]);
                result.push_back(info);
            }
        }
    }

    return result;
}

MiscDbModResult MakeAllCarsAutoshow() {
    return ExecuteMiscDbMod({
        {
            "Backup autoshow state",
            "CREATE TABLE IF NOT EXISTS _backup_AutoshowState AS "
            "SELECT Id, NotAvailableInAutoshow, BaseCost FROM Data_Car"
        },
        {
            "Set autoshow availability",
            "UPDATE Data_Car SET NotAvailableInAutoshow = 0"
        },
        {
            "Drop Drivable_Data_Car view",
            "DROP VIEW IF EXISTS Drivable_Data_Car"
        },
        {
            "Recreate Drivable_Data_Car view",
            "CREATE VIEW Drivable_Data_Car AS SELECT Data_Car.* FROM Data_Car WHERE Id NOT IN (SELECT Ordinal FROM UnobtainableCars)"
        },
        {
            "Backup car bucket map",
            "CREATE TABLE IF NOT EXISTS _backup_DataCarBuckets AS "
            "SELECT * FROM Data_Car_Buckets"
        },
        {
            "Add missing car bucket rows",
            R"sql(
INSERT OR IGNORE INTO Data_Car_Buckets(CarId, CarBucket, BucketHero)
SELECT Id,
       COALESCE((SELECT CarBucket FROM Data_Car_Buckets GROUP BY CarBucket ORDER BY COUNT(*) DESC LIMIT 1), 1),
       0
FROM Data_Car
WHERE Id NOT IN (SELECT CarId FROM Data_Car_Buckets)
)sql"
        },
        {
            "Normalize car bucket rows",
            R"sql(
UPDATE Data_Car_Buckets
SET CarBucket = COALESCE(CarBucket, (SELECT CarBucket FROM Data_Car_Buckets GROUP BY CarBucket ORDER BY COUNT(*) DESC LIMIT 1), 1),
    BucketHero = COALESCE(BucketHero, 0)
WHERE CarBucket IS NULL OR BucketHero IS NULL
)sql"
        }
    }, "All database cars were made available in Autoshow");
}

MiscDbModResult MakeAllCarsFree() {
    return ExecuteMiscDbMod({
        {
            "Backup car prices",
            "CREATE TABLE IF NOT EXISTS _backup_CarPrices AS SELECT Id, BaseCost FROM Data_Car"
        },
        {
            "Set all car prices to 0",
            "UPDATE Data_Car SET BaseCost = 0"
        }
    }, "All car prices were set to 0 CR");
}

MiscDbModResult ClearNewGarageTags() {
    return ExecuteMiscDbMod({
        {
            "Mark garage cars viewed",
            "UPDATE Profile0_Career_Garage SET HasCurrentOwnerViewedCar = 1"
        }
    }, "New tags were cleared from garage cars");
}

MiscDbModResult MakeUpgradesFree() {
    const char* upgradeTables[] = {
        "List_UpgradeAntiSwayFront",
        "List_UpgradeAntiSwayRear",
        "List_UpgradeBrakes",
        "List_UpgradeCarBodyChassisStiffness",
        "List_UpgradeCarBody",
        "List_UpgradeCarBodyTireAspectRatioFront",
        "List_UpgradeCarBodyTireAspectRatioRear",
        "List_UpgradeCarBodyTireWidthFront",
        "List_UpgradeCarBodyTireWidthRear",
        "List_UpgradeCarBodyTrackSpacingFront",
        "List_UpgradeCarBodyTrackSpacingRear",
        "List_UpgradeCarBodyWeight",
        "List_UpgradeDrivetrain",
        "List_UpgradeDrivetrainClutch",
        "List_UpgradeDrivetrainDifferential",
        "List_UpgradeDrivetrainDriveline",
        "List_UpgradeDrivetrainTransmission",
        "List_UpgradeEngine",
        "List_UpgradeEngineCamshaft",
        "List_UpgradeEngineCSC",
        "List_UpgradeEngineDisplacement",
        "List_UpgradeEngineDSC",
        "List_UpgradeEngineExhaust",
        "List_UpgradeEngineFlywheel",
        "List_UpgradeEngineFuelSystem",
        "List_UpgradeEngineIgnition",
        "List_UpgradeEngineIntake",
        "List_UpgradeEngineIntercooler",
        "List_UpgradeEngineManifold",
        "List_UpgradeEngineOilCooling",
        "List_UpgradeEnginePistonsCompression",
        "List_UpgradeEngineRestrictorPlate",
        "List_UpgradeEngineTurboQuad",
        "List_UpgradeEngineTurboSingle",
        "List_UpgradeEngineTurboTwin",
        "List_UpgradeEngineValves",
        "List_UpgradeMotor",
        "List_UpgradeMotorParts",
        "List_UpgradeRimSizeFront",
        "List_UpgradeRimSizeRear",
        "List_UpgradeSpringDamper",
        "List_UpgradeTireCompound",
        "List_UpgradeCarBodyFrontBumper",
        "List_UpgradeCarBodyHood",
        "List_UpgradeCarBodyRearBumper",
        "List_UpgradeCarBodySideSkirt",
        "List_UpgradeRearWing",
    };

    std::vector<SqlStep> steps;
    steps.reserve((sizeof(upgradeTables) / sizeof(upgradeTables[0])) + 2);

    for (const char* table : upgradeTables) {
        steps.push_back({
            std::string("Set ") + table + " prices to 0",
            std::string("UPDATE [") + table + "] SET Price = 0"
        });
    }

    steps.push_back({
        "Set wheel prices to 1",
        "UPDATE List_Wheels SET Price = 1"
    });
    steps.push_back({
        "Unlock hidden upgrade presets",
        "UPDATE UpgradePresetPackages SET Purchasable = 1 WHERE Purchasable = 0"
    });

    return ExecuteMiscDbMod(steps, "Upgrade prices were updated and hidden presets were unlocked");
}

FovOverrideResult ApplyFovOverride(float targetFov, bool rescanTargets) {
    FovOverrideResult outcome;

    auto proc = process::open_process(L"forzahorizon6.exe");
    if (!proc || !proc->handle) {
        outcome.message = "Start FH6 first, then run this tool as administrator.";
        return outcome;
    }

    if (rescanTargets || g_fovTargets.empty()) {
        int rawCandidateCount = 0;
        g_fovTargets = DiscoverFovTargets(proc->handle, rawCandidateCount);
        outcome.candidates = rawCandidateCount;
    }

    outcome = WriteFovTargets(proc->handle, targetFov);
    process::close_process(*proc);
    return outcome;
}

void ClearFovOverrideTargets() {
    g_fovTargets.clear();
}
