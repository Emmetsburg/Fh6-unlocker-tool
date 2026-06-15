#pragma once

#include <string>
#include <vector>

struct TargetedCarUnlockResult {
    bool success = false;
    bool alreadyOwned = false;
    int ownedCount = 0;
    int garageId = 0;
    int replacedCarId = 0;
    std::string message;
};

struct GarageCarInfo {
    int carId = 0;
    int garageId = 0;          // Profile0_Career_Garage.Id (row id)
};

struct MiscDbModResult {
    bool success = false;
    int succeeded = 0;
    int failed = 0;
    std::string message;
};

struct FovOverrideResult {
    bool success = false;
    int candidates = 0;
    int written = 0;
    int verified = 0;
    std::string message;
};

TargetedCarUnlockResult GrantCarToGarage(int carId, bool allowDuplicate = false, bool prepareForGarage = false);
TargetedCarUnlockResult SetTemporaryLeadPlayerCar(int carId);
TargetedCarUnlockResult BorrowCarOverGarageRow(int carId, int garageId, bool prepareForGarage = true);
TargetedCarUnlockResult RestoreBorrowedGarageRow(int garageId);
TargetedCarUnlockResult RemoveGarageRowById(int garageId);

// Returns list of carIds currently in the garage, or empty vector on failure.
// outError is set on failure.
std::vector<GarageCarInfo> GetGarageCarIds(std::string& outError);

MiscDbModResult MakeAllCarsAutoshow();
MiscDbModResult MakeAllCarsFree();
MiscDbModResult ClearNewGarageTags();
MiscDbModResult MakeUpgradesFree();
FovOverrideResult ApplyFovOverride(float targetFov, bool rescanTargets = true);
void ClearFovOverrideTargets();
