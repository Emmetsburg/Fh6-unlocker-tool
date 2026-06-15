#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <string>
#include <vector>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <nlohmann/json.hpp>
#include "data/fonts.h"
#include "glass/glass.h"
#include "Injector.h"
#include "TargetedCarUnlock.h"

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using json = nlohmann::json;

struct Car {
    int id;
    int year;
    std::string make;
    std::string model;
    std::string name;
    std::string displayName;
    int baseCost = 0;
    int pi = 0;
    bool isDrivable = true;
    bool notAvailableInAutoshow = false;
    bool visibleOnlyIfOwned = false;
    std::vector<std::string> aliases;
};

HWND g_hWnd = nullptr;
ID3D11Device* g_pd3dDevice = nullptr;
ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
IDXGISwapChain* g_pSwapChain = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
Glass::Renderer g_glassRenderer;
ImFont* g_fontSemibold = nullptr;
const wchar_t* kWindowClassName = L"FH6CarToolWindow";
const wchar_t* kWindowTitle = L"FH6 Car Tool";
constexpr int kCarsJsonResourceId = 101;
constexpr int kAppIconResourceId = 104;
constexpr int kFontAwesomeSolidResourceId = 105;
constexpr int kFontAwesomeBrandsResourceId = 106;
constexpr float kWindowControlSize = 34.0f;
constexpr float kWindowControlGap = 6.0f;

enum class PendingMiscAction {
    None,
    AllCarsAutoshow,
    FreeCars,
    FreeUpgrades,
    ClearNewTags
};

struct AppState {
    bool processAttached = false;
    int activePage = 0;
    std::string statusMessage;
    float statusTimer = 0.0f;

    char searchBuffer[256] = "";
    std::string lastSearchQuery;
    int selectedCarIndex = -1;
    std::vector<Car> filteredCars;
    bool showDuplicateAddConfirm = false;
    int pendingDuplicateCarId = 0;
    int pendingDuplicateCount = 0;
    int temporaryGarageRowId = 0;
    std::string temporaryCarName;
    int borrowedGarageRowId = 0;
    int borrowedOriginalCarId = 0;
    std::string borrowedOriginalCarName;
    std::string borrowedCarName;

    char trafficSearchBuffer[256] = "";
    int trafficSelectedIndex = -1;

    struct GarageCar {
        int carId = 0;
        int garageId = 0;
        std::string displayName;
    };
    std::vector<GarageCar> garageCars;
    int garageSelectedIndex = -1;
    bool garageLoaded = false;
    std::string garageErrorMessage;
    char garageSearchBuffer[256] = "";
    bool showRemoveConfirm = false;

    PendingMiscAction pendingMiscAction = PendingMiscAction::None;
    float fovValue = 90.0f;
    bool fovKeepApplied = false;
    float fovReapplyTimer = 0.0f;
    int fovLastVerified = 0;
    
    Injector injector;
} g_appState;

enum class StatusKind {
    Info,
    Success,
    Error
};

StatusKind g_statusKind = StatusKind::Info;

class CarDatabase {
public:
    static CarDatabase& GetInstance();

    bool Load();
    std::vector<Car> SearchCars(const std::string& query);
    Car* GetCarById(int id);

    const std::vector<Car>& GetAllCars() const { return cars; }
    size_t GetCarCount() const { return cars.size(); }

private:
    CarDatabase() {}
    ~CarDatabase() {}
    CarDatabase(const CarDatabase&) = delete;
    CarDatabase& operator=(const CarDatabase&) = delete;

    std::vector<Car> cars;
    std::map<int, Car*> idMap;

    bool LoadFromJSON();
    void BuildIndexes();
};

CarDatabase& CarDatabase::GetInstance() {
    static CarDatabase instance;
    return instance;
}

bool CarDatabase::Load() {
    if (!LoadFromJSON()) {
        return false;
    }
    BuildIndexes();
    return true;
}

struct EmbeddedResourceView {
    const void* data = nullptr;
    size_t size = 0;
};

std::optional<EmbeddedResourceView> LoadEmbeddedResource(int resourceId) {
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!resource) {
        return std::nullopt;
    }

    HGLOBAL loadedResource = LoadResource(nullptr, resource);
    if (!loadedResource) {
        return std::nullopt;
    }

    const DWORD size = SizeofResource(nullptr, resource);
    const void* data = LockResource(loadedResource);
    if (!data || size == 0) {
        return std::nullopt;
    }

    return EmbeddedResourceView{ data, static_cast<size_t>(size) };
}

std::optional<std::string> LoadEmbeddedCarsJson() {
    auto resource = LoadEmbeddedResource(kCarsJsonResourceId);
    if (!resource) {
        return std::nullopt;
    }

    return std::string(static_cast<const char*>(resource->data), resource->size);
}

std::optional<std::string> LoadCarsJsonText() {
    if (auto embedded = LoadEmbeddedCarsJson()) {
        return embedded;
    }

    std::ifstream file("cars.json", std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool CarDatabase::LoadFromJSON() {
    auto jsonText = LoadCarsJsonText();
    if (!jsonText) {
        return false;
    }

    try {
        json j = json::parse(*jsonText);

        if (!j.contains("cars")) {
            return false;
        }

        cars.clear();
        for (const auto& carJson : j["cars"]) {
            Car car;
            car.id = carJson["id"];
            car.year = carJson.value("year", 0);
            car.make = carJson.value("make", "");
            car.model = carJson.value("model", "");
            car.name = carJson.value("name", "");
            car.displayName = carJson.value("displayName", "");
            car.baseCost = carJson.value("baseCost", 0);
            car.pi = carJson.value("pi", 0);
            car.isDrivable = carJson.value("isDrivable", true);
            car.notAvailableInAutoshow = carJson.value("notAvailableInAutoshow", false);
            car.visibleOnlyIfOwned = carJson.value("visibleOnlyIfOwned", false);
            if (carJson.contains("aliases") && carJson["aliases"].is_array()) {
                for (const auto& aliasJson : carJson["aliases"]) {
                    if (aliasJson.is_string()) {
                        car.aliases.push_back(aliasJson.get<std::string>());
                    }
                }
            }
            cars.push_back(car);
        }

        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void CarDatabase::BuildIndexes() {
    idMap.clear();

    for (auto& car : cars) {
        idMap[car.id] = &car;
    }
}

static std::string ToLowerCopy(const std::string& value) {
    std::string result = value;
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char ch) { return (char)std::tolower(ch); });
    return result;
}

static std::string NormalizeSearchText(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (unsigned char ch : value) {
        if (std::isalnum(ch)) {
            result.push_back((char)std::tolower(ch));
        }
    }
    return result;
}

static bool ContainsAnySearchFragment(const std::string& lowerText, const std::string& normalizedText,
    std::initializer_list<const char*> terms) {
    for (const char* term : terms) {
        const std::string lowerTerm = ToLowerCopy(term);
        if (lowerText.find(lowerTerm) != std::string::npos) {
            return true;
        }

        const std::string normalizedTerm = NormalizeSearchText(term);
        if (!normalizedTerm.empty() && normalizedText.find(normalizedTerm) != std::string::npos) {
            return true;
        }
    }

    return false;
}

static std::string BuildSearchBlob(const Car& car) {
    std::ostringstream blob;
    blob << car.displayName << ' '
         << car.name << ' '
         << car.make << ' '
         << car.model << ' '
         << "PI " << car.pi << ' '
         << car.year << ' '
         << car.id;

    for (const auto& alias : car.aliases) {
        blob << ' ' << alias;
    }

    const std::string classText = blob.str();
    const std::string lowerClassText = ToLowerCopy(classText);
    const std::string normalizedClassText = NormalizeSearchText(classText);

    if (car.notAvailableInAutoshow) {
        blob << " offsale hidden not autoshow rare secret";
    }
    if (car.visibleOnlyIfOwned) {
        blob << " visible owned";
    }
    if (!car.isDrivable) {
        blob << " not drivable traffic npc";
    }
    if (ContainsAnySearchFragment(lowerClassText, normalizedClassText,
        { "traffic", "taxi traffic", "wrangler traffic", "pajero traffic" })) {
        blob << " traffic npc ai civilian street";
    }
    if (ContainsAnySearchFragment(lowerClassText, normalizedClassText,
        { "truck", "pickup", "f150", "f-150", "f250", "f-250", "f450", "f-450",
          "ramsrt", "ram srt", "ramtrx", "ram trx", "trx", "tacoma", "silverado", "syclone", "trophytruck", "trophy truck",
          "ckpickup", "ck pickup", "gladiator", "r1t", "acty", "minicab" })) {
        blob << " truck pickup ute lorry";
    }
    if (ContainsAnySearchFragment(lowerClassText, normalizedClassText,
        { "wrangler", "bronco", "defender", "pajero", "g65", "g 65", "landcruiser",
          "land cruiser", "4runner", "hummer", "dbx", "durango", "x5", "x6",
          "bmw ix", "gladiator", "r1t" })) {
        blob << " suv offroad 4x4";
    }
    if (ContainsAnySearchFragment(lowerClassText, normalizedClassText,
        { "van", "transit", "supervan", "hongguang", "acty", "minicab", "scargo", "caddy" })) {
        blob << " van minivan utility";
    }
    if (ContainsAnySearchFragment(lowerClassText, normalizedClassText,
        { "buggy", "rzr", "pro4", "class1", "class6100", "hammerhead", "trophy", "dakar", "manx" })) {
        blob << " buggy offroad";
    }
    if (ContainsAnySearchFragment(lowerClassText, normalizedClassText, { "taxi" })) {
        blob << " taxi cab";
    }

    return blob.str();
}

static bool MatchesSearch(const Car& car, const std::string& lowerQuery, const std::string& normalizedQuery) {
    const std::string blob = BuildSearchBlob(car);
    const std::string lowerBlob = ToLowerCopy(blob);
    if (lowerBlob.find(lowerQuery) != std::string::npos) {
        return true;
    }

    if (!normalizedQuery.empty()) {
        const std::string normalizedBlob = NormalizeSearchText(blob);
        return normalizedBlob.find(normalizedQuery) != std::string::npos;
    }

    return false;
}

static bool IsTrafficVehicle(const Car& car) {
    if (!car.isDrivable) {
        return true;
    }

    std::ostringstream text;
    text << car.displayName << ' '
         << car.name << ' '
         << car.make << ' '
         << car.model;
    for (const auto& alias : car.aliases) {
        text << ' ' << alias;
    }

    const std::string value = text.str();
    const std::string lowerValue = ToLowerCopy(value);
    const std::string normalizedValue = NormalizeSearchText(value);
    return ContainsAnySearchFragment(lowerValue, normalizedValue, { "traffic" });
}

static std::vector<Car> GetFilteredTrafficVehicles(const std::string& query) {
    std::vector<Car> result;
    const std::string lowerQuery = ToLowerCopy(query);
    const std::string normalizedQuery = NormalizeSearchText(query);

    for (const auto& car : CarDatabase::GetInstance().GetAllCars()) {
        if (!IsTrafficVehicle(car)) {
            continue;
        }

        if (lowerQuery.empty() || MatchesSearch(car, lowerQuery, normalizedQuery)) {
            result.push_back(car);
        }
    }

    return result;
}

std::vector<Car> CarDatabase::SearchCars(const std::string& query) {
    std::vector<Car> results;
    std::string lowerQuery = ToLowerCopy(query);
    std::string normalizedQuery = NormalizeSearchText(query);

    for (const auto& car : cars) {
        if (IsTrafficVehicle(car)) {
            continue;
        }

        if (lowerQuery.empty() || MatchesSearch(car, lowerQuery, normalizedQuery)) {
            results.push_back(car);
        }
    }

    return results;
}

Car* CarDatabase::GetCarById(int id) {
    auto it = idMap.find(id);
    if (it != idMap.end()) {
        return it->second;
    }
    return nullptr;
}

void SetStatus(const std::string& message, StatusKind kind, float seconds) {
    g_appState.statusMessage = message;
    g_appState.statusTimer = seconds;
    g_statusKind = kind;
}

void ClearDuplicateAddPrompt() {
    g_appState.showDuplicateAddConfirm = false;
    g_appState.pendingDuplicateCarId = 0;
    g_appState.pendingDuplicateCount = 0;
}

void ClearTemporaryBorrowState() {
    g_appState.temporaryGarageRowId = 0;
    g_appState.temporaryCarName.clear();
}

void ClearBorrowedGarageState() {
    g_appState.borrowedGarageRowId = 0;
    g_appState.borrowedOriginalCarId = 0;
    g_appState.borrowedOriginalCarName.clear();
    g_appState.borrowedCarName.clear();
}

bool RefreshGarageLibraryFromGame() {
    std::string err;
    auto rawList = GetGarageCarIds(err);
    g_appState.garageCars.clear();
    g_appState.garageSelectedIndex = -1;
    g_appState.showRemoveConfirm = false;

    if (!err.empty()) {
        g_appState.garageErrorMessage = err;
        g_appState.garageLoaded = false;
        return false;
    }

    auto& db = CarDatabase::GetInstance();
    for (const auto& raw : rawList) {
        AppState::GarageCar gc;
        gc.carId = raw.carId;
        gc.garageId = raw.garageId;
        Car* c = db.GetCarById(raw.carId);
        gc.displayName = c ? c->displayName : ("[Unknown ID " + std::to_string(raw.carId) + "]");
        g_appState.garageCars.push_back(gc);
    }

    g_appState.garageErrorMessage.clear();
    g_appState.garageLoaded = true;
    return true;
}

void RemoveTemporaryBorrowedCar() {
    if (g_appState.temporaryGarageRowId <= 0) {
        return;
    }

    const std::string temporaryName = g_appState.temporaryCarName.empty()
        ? "temporary car"
        : g_appState.temporaryCarName;
    auto result = RemoveGarageRowById(g_appState.temporaryGarageRowId);
    if (result.success) {
        SetStatus("Removed temporary " + temporaryName + " garage row.", StatusKind::Success, 5.0f);
        ClearTemporaryBorrowState();
        g_appState.garageLoaded = false;
    } else {
        SetStatus("Temporary remove failed: " + result.message, StatusKind::Error, 6.0f);
    }
}

void AddTemporaryGarageRow(const Car& car) {
    if (g_appState.temporaryGarageRowId > 0) {
        SetStatus("Remove the current temporary row before adding another one.", StatusKind::Info, 5.0f);
        return;
    }

    auto result = GrantCarToGarage(car.id, true, IsTrafficVehicle(car));
    if (result.success && result.garageId > 0) {
        ClearDuplicateAddPrompt();
        g_appState.temporaryGarageRowId = result.garageId;
        g_appState.temporaryCarName = car.displayName;
        g_appState.garageLoaded = false;
        if (IsTrafficVehicle(car)) {
            SetStatus("Prepared temporary traffic row added as row " + std::to_string(result.garageId) + ". Reopen garage/car select if it does not refresh.", StatusKind::Success, 8.0f);
        } else {
            SetStatus("Temporary garage row added as row " + std::to_string(result.garageId) + ". This does not switch your current car.", StatusKind::Success, 7.0f);
        }
    } else if (result.success) {
        g_appState.garageLoaded = false;
        SetStatus("Temporary row was added, but its garage row ID could not be detected. Use My Garage to remove it.", StatusKind::Error, 7.0f);
    } else {
        SetStatus("Temporary row failed: " + result.message, StatusKind::Error, 6.0f);
    }
}

void SetTemporaryCurrentCar(const Car& car) {
    auto result = SetTemporaryLeadPlayerCar(car.id);
    if (result.success) {
        ClearDuplicateAddPrompt();
        SetStatus("Temp borrow set to " + car.displayName + ". Exit the pause menu to let FH switch the current car.", StatusKind::Success, 8.0f);
    } else {
        SetStatus("Temp borrow failed: " + result.message, StatusKind::Error, 7.0f);
    }
}

void RestoreBorrowedGarageRowFromGame() {
    if (g_appState.borrowedGarageRowId <= 0) {
        return;
    }

    const std::string borrowedName = g_appState.borrowedCarName.empty()
        ? "replacement car"
        : g_appState.borrowedCarName;
    const std::string originalName = g_appState.borrowedOriginalCarName.empty()
        ? "original car"
        : g_appState.borrowedOriginalCarName;

    auto result = RestoreBorrowedGarageRow(g_appState.borrowedGarageRowId);
    if (result.success) {
        SetStatus("Restored " + originalName + " over " + borrowedName + ".", StatusKind::Success, 6.0f);
        ClearBorrowedGarageState();
        g_appState.garageLoaded = false;
    } else {
        SetStatus("Restore failed: " + result.message, StatusKind::Error, 7.0f);
    }
}

void BorrowOverSelectedGarageRow(const Car& car) {
    if (g_appState.borrowedGarageRowId > 0) {
        SetStatus("Restore the current replaced row before replacing another car.", StatusKind::Info, 5.0f);
        return;
    }

    if (!g_appState.garageLoaded ||
        g_appState.garageSelectedIndex < 0 ||
        g_appState.garageSelectedIndex >= (int)g_appState.garageCars.size()) {
        SetStatus("Select one of your cars in My Garage first, then replace that row.", StatusKind::Info, 6.0f);
        return;
    }

    const auto target = g_appState.garageCars[g_appState.garageSelectedIndex];
    if (target.garageId == g_appState.temporaryGarageRowId) {
        SetStatus("Select a normal garage row, not the temporary row, before replacing.", StatusKind::Info, 5.0f);
        return;
    }

    auto result = BorrowCarOverGarageRow(car.id, target.garageId, IsTrafficVehicle(car));
    if (result.success) {
        g_appState.borrowedGarageRowId = target.garageId;
        g_appState.borrowedOriginalCarId = target.carId;
        g_appState.borrowedOriginalCarName = target.displayName;
        g_appState.borrowedCarName = car.displayName;
        g_appState.garageLoaded = false;
        SetStatus("Replaced " + target.displayName + " row with " + car.displayName + ". Reopen garage/car select if it does not refresh.", StatusKind::Success, 9.0f);
    } else {
        SetStatus("Replace failed: " + result.message, StatusKind::Error, 7.0f);
    }
}

void RefreshCarSearch() {
    std::string query(g_appState.searchBuffer);
    const bool queryChanged = query != g_appState.lastSearchQuery;
    if (!queryChanged && !g_appState.filteredCars.empty()) {
        return;
    }

    if (queryChanged) {
        ClearDuplicateAddPrompt();
    }

    g_appState.filteredCars = CarDatabase::GetInstance().SearchCars(query);
    g_appState.lastSearchQuery = query;

    if (g_appState.selectedCarIndex >= (int)g_appState.filteredCars.size()) {
        g_appState.selectedCarIndex = -1;
        ClearDuplicateAddPrompt();
    }
}

void CheckGameProcess() {
    if (g_appState.injector.FindProcess("forzahorizon6.exe")) {
        g_appState.processAttached = true;
        SetStatus("FH6 is running. Select a car and press Add Car.", StatusKind::Success, 3.0f);
    } else {
        g_appState.processAttached = false;
        SetStatus("Could not find running forzahorizon6.exe.", StatusKind::Error, 3.0f);
    }
}

const char* MiscActionTitle(PendingMiscAction action) {
    switch (action) {
    case PendingMiscAction::AllCarsAutoshow:
        return "All Cars in Autoshow";
    case PendingMiscAction::FreeCars:
        return "Free Cars";
    case PendingMiscAction::FreeUpgrades:
        return "Free Upgrades";
    case PendingMiscAction::ClearNewTags:
        return "Clear New Tags";
    default:
        return "";
    }
}

const char* MiscActionDescription(PendingMiscAction action) {
    switch (action) {
    case PendingMiscAction::AllCarsAutoshow:
        return "This sets Autoshow availability flags, rebuilds the drivable view, and fills missing car bucket rows.";
    case PendingMiscAction::FreeCars:
        return "This backs up Data_Car prices once, then sets BaseCost to 0 for every car.";
    case PendingMiscAction::FreeUpgrades:
        return "This sets upgrade prices to 0, wheel prices to 1, and marks upgrade presets purchasable.";
    case PendingMiscAction::ClearNewTags:
        return "This marks every garage car as already viewed.";
    default:
        return "";
    }
}

MiscDbModResult RunMiscAction(PendingMiscAction action) {
    switch (action) {
    case PendingMiscAction::AllCarsAutoshow:
        return MakeAllCarsAutoshow();
    case PendingMiscAction::FreeCars:
        return MakeAllCarsFree();
    case PendingMiscAction::FreeUpgrades:
        return MakeUpgradesFree();
    case PendingMiscAction::ClearNewTags:
        return ClearNewGarageTags();
    default:
        return {};
    }
}

void QueueMiscAction(PendingMiscAction action) {
    g_appState.pendingMiscAction = action;
    SetStatus(std::string(MiscActionTitle(action)) + " queued. Confirm it in the Misc tab.", StatusKind::Info, 5.0f);
}

void ExecuteQueuedMiscAction() {
    const PendingMiscAction action = g_appState.pendingMiscAction;
    if (action == PendingMiscAction::None) {
        return;
    }

    auto result = RunMiscAction(action);
    g_appState.pendingMiscAction = PendingMiscAction::None;
    g_appState.garageLoaded = false;

    if (result.success) {
        SetStatus(std::string(MiscActionTitle(action)) + ": " + result.message, StatusKind::Success, 6.0f);
    } else {
        SetStatus(std::string(MiscActionTitle(action)) + " failed: " + result.message, StatusKind::Error, 7.0f);
    }
}

void ApplyFovFromUi(bool rescanTargets) {
    auto result = ApplyFovOverride(g_appState.fovValue, rescanTargets);
    g_appState.fovLastVerified = result.verified;
    if (result.success) {
        g_appState.fovKeepApplied = true;
        g_appState.fovReapplyTimer = 0.35f;
        SetStatus(result.message, StatusKind::Success, 6.0f);
    } else {
        g_appState.fovKeepApplied = false;
        SetStatus("FOV failed: " + result.message, StatusKind::Error, 7.0f);
    }
}

void TickFovOverride(float deltaSeconds) {
    if (!g_appState.fovKeepApplied) {
        return;
    }

    g_appState.fovReapplyTimer -= deltaSeconds;
    if (g_appState.fovReapplyTimer > 0.0f) {
        return;
    }

    g_appState.fovReapplyTimer = 0.35f;
    auto result = ApplyFovOverride(g_appState.fovValue, false);
    g_appState.fovLastVerified = result.verified;
    if (!result.success) {
        g_appState.fovKeepApplied = false;
        ClearFovOverrideTargets();
        SetStatus("FOV keep-applied stopped: " + result.message, StatusKind::Error, 7.0f);
    }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
bool CreateRenderTarget();
void CleanupRenderTarget();
void InitImGui();
void RenderGUI();

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    if (!CarDatabase::GetInstance().Load()) {
        MessageBoxA(nullptr, "Failed to load embedded car database", "Error", MB_ICONERROR);
        return 1;
    }

    HINSTANCE moduleInstance = GetModuleHandleW(nullptr);
    HICON appIcon = LoadIconW(moduleInstance, MAKEINTRESOURCEW(kAppIconResourceId));
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW), CS_CLASSDC, WndProc, 0L, 0L, moduleInstance, appIcon, nullptr, nullptr, nullptr, kWindowClassName, appIcon };
    RegisterClassExW(&wc);

    const int windowWidth = 1180;
    const int windowHeight = 800;
    const int windowX = (GetSystemMetrics(SM_CXSCREEN) - windowWidth) / 2;
    const int windowY = (GetSystemMetrics(SM_CYSCREEN) - windowHeight) / 2;
    g_hWnd = CreateWindowExW(WS_EX_LAYERED, wc.lpszClassName, kWindowTitle, WS_POPUP,
        windowX, windowY, windowWidth, windowHeight, nullptr, nullptr, wc.hInstance, nullptr);
    SetWindowTextW(g_hWnd, kWindowTitle);
    SendMessageW(g_hWnd, WM_SETICON, ICON_BIG, (LPARAM)appIcon);
    SendMessageW(g_hWnd, WM_SETICON, ICON_SMALL, (LPARAM)appIcon);
    SetLayeredWindowAttributes(g_hWnd, RGB(0, 0, 0), 255, LWA_ALPHA);
    MARGINS margins = { -1 };
    DwmExtendFrameIntoClientArea(g_hWnd, &margins);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext) != S_OK) {
        MessageBoxA(nullptr, "Failed to create Direct3D device", "Error", MB_ICONERROR);
        return 1;
    }

    if (!CreateRenderTarget()) {
        MessageBoxA(nullptr, "Failed to create Direct3D render target", "Error", MB_ICONERROR);
        return 1;
    }
    InitImGui();
    if (!g_glassRenderer.Init(g_pd3dDevice, g_pd3dDeviceContext)) {
        MessageBoxA(g_hWnd, "Liquid Glass renderer initialization failed.", "Error", MB_ICONERROR);
        return 1;
    }
    Glass::g = &g_glassRenderer;
    Glass::GlassEdgeConfig edgeConfig;
    edgeConfig.cursor_size = 1.0f;
    edgeConfig.cursor_glow = 0.0f;
    g_glassRenderer.SetEdgeConfig(edgeConfig);
    ShowWindow(g_hWnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hWnd);

    bool done = false;
    MSG msg = {};
    while (!done) {
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                done = true;
            }
        }
        if (done) {
            break;
        }
        if (IsIconic(g_hWnd)) {
            Sleep(10);
            continue;
        }

        RECT clientRect = {};
        GetClientRect(g_hWnd, &clientRect);
        const int clientWidth = clientRect.right - clientRect.left;
        const int clientHeight = clientRect.bottom - clientRect.top;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const ImVec2 cursorLocal(-100000.0f, -100000.0f);
        g_glassRenderer.BeginFrame(
            clientWidth,
            clientHeight,
            0,
            0,
            clientWidth,
            clientHeight,
            cursorLocal);

        RenderGUI();

        ImGui::Render();
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
        D3D11_VIEWPORT viewport = {};
        viewport.Width = (float)clientWidth;
        viewport.Height = (float)clientHeight;
        viewport.MaxDepth = 1.0f;
        g_pd3dDeviceContext->RSSetViewports(1, &viewport);
        g_glassRenderer.Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0);
    }

    g_glassRenderer.Shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); }
    DestroyWindow(g_hWnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}

void ApplyMaximizedWorkArea(HWND hWnd, LPARAM lParam) {
    auto* minMaxInfo = reinterpret_cast<MINMAXINFO*>(lParam);
    const HMONITOR monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);

    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo)) {
        return;
    }

    const RECT& workArea = monitorInfo.rcWork;
    const RECT& monitorArea = monitorInfo.rcMonitor;
    minMaxInfo->ptMaxPosition.x = workArea.left - monitorArea.left;
    minMaxInfo->ptMaxPosition.y = workArea.top - monitorArea.top;
    minMaxInfo->ptMaxSize.x = workArea.right - workArea.left;
    minMaxInfo->ptMaxSize.y = workArea.bottom - workArea.top;
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_GETMINMAXINFO:
        ApplyMaximizedWorkArea(hWnd, lParam);
        return 0;
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            PostMessageW(hWnd, WM_CLOSE, 0, 0);
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

bool CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    if (!g_pSwapChain || FAILED(g_pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer)) || !pBackBuffer) {
        return false;
    }

    const HRESULT hr = g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
    return SUCCEEDED(hr) && g_mainRenderTargetView != nullptr;
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

void SetupModernTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;
    auto frost = [](float alpha) { return ImVec4(1.0f, 1.0f, 1.0f, alpha); };

    colors[ImGuiCol_Text]                 = ImVec4(0.925f, 0.929f, 0.953f, 1.00f);
    colors[ImGuiCol_TextDisabled]         = ImVec4(0.592f, 0.604f, 0.659f, 1.00f);
    colors[ImGuiCol_WindowBg]             = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_ChildBg]              = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_Border]               = frost(0.07f);
    colors[ImGuiCol_Separator]            = frost(0.10f);
    colors[ImGuiCol_Button]               = frost(0.06f);
    colors[ImGuiCol_ButtonHovered]        = frost(0.12f);
    colors[ImGuiCol_ButtonActive]         = frost(0.18f);
    colors[ImGuiCol_FrameBg]              = frost(0.06f);
    colors[ImGuiCol_FrameBgHovered]       = frost(0.10f);
    colors[ImGuiCol_FrameBgActive]        = frost(0.14f);
    colors[ImGuiCol_Header]               = frost(0.10f);
    colors[ImGuiCol_HeaderHovered]        = frost(0.14f);
    colors[ImGuiCol_HeaderActive]         = frost(0.18f);
    colors[ImGuiCol_CheckMark]            = ImVec4(1.0f, 1.0f, 1.0f, 0.95f);
    colors[ImGuiCol_SliderGrab]           = frost(0.55f);
    colors[ImGuiCol_SliderGrabActive]     = frost(0.75f);
    colors[ImGuiCol_PopupBg]              = ImVec4(0.10f, 0.105f, 0.125f, 0.94f);
    colors[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_ScrollbarGrab]        = frost(0.16f);
    colors[ImGuiCol_ScrollbarGrabHovered] = frost(0.24f);
    colors[ImGuiCol_ScrollbarGrabActive]  = frost(0.30f);
    colors[ImGuiCol_Tab]                  = frost(0.05f);
    colors[ImGuiCol_TabHovered]           = frost(0.12f);
    colors[ImGuiCol_TabActive]            = frost(0.16f);

    style.WindowRounding    = 0.0f;
    style.ChildRounding     = 13.0f;
    style.FrameRounding     = 10.0f;
    style.GrabRounding      = 10.0f;
    style.PopupRounding     = 13.0f;
    style.ScrollbarRounding = 12.0f;
    style.TabRounding       = 10.0f;
    style.WindowBorderSize  = 0.0f;
    style.ChildBorderSize   = 0.0f;
    style.FrameBorderSize   = 0.0f;
    style.PopupBorderSize   = 0.0f;
    style.ScrollbarSize     = 10.0f;
    style.WindowPadding     = ImVec2(12.0f, 12.0f);
    style.FramePadding      = ImVec2(10.0f, 7.0f);
    style.ItemSpacing       = ImVec2(9.0f, 7.0f);
    style.ItemInnerSpacing  = ImVec2(7.0f, 5.0f);
    style.AntiAliasedLines = true;
    style.AntiAliasedLinesUseTex = true;
    style.AntiAliasedFill = true;
    style.CircleTessellationMaxError = 0.10f;
    style.CurveTessellationTol = 0.80f;
}

void LoadLiquidGlassFonts() {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig fontConfig;
    fontConfig.FontDataOwnedByAtlas = false;
    ImFont* fontMedium = io.Fonts->AddFontFromMemoryTTF(inter_medium.data(), (int)inter_medium.size(), 17.0f, &fontConfig);
    g_fontSemibold = io.Fonts->AddFontFromMemoryTTF(inter_semibold.data(), (int)inter_semibold.size(), 24.0f, &fontConfig);
    if (fontMedium) {
        io.FontDefault = fontMedium;
    }

    static const ImWchar iconRanges[] = { 0xE000, 0xF8FF, 0 };
    ImFontConfig iconConfig;
    iconConfig.FontDataOwnedByAtlas = false;
    iconConfig.OversampleH = 2;
    iconConfig.OversampleV = 2;

    if (auto solidResource = LoadEmbeddedResource(kFontAwesomeSolidResourceId)) {
        ImFont* solid = io.Fonts->AddFontFromMemoryTTF(
            const_cast<void*>(solidResource->data),
            (int)solidResource->size,
            32.0f,
            &iconConfig,
            iconRanges);
        if (solid) {
            Glass::SetIconFont(solid);
        }
    }

    if (auto brandsResource = LoadEmbeddedResource(kFontAwesomeBrandsResourceId)) {
        ImFont* brands = io.Fonts->AddFontFromMemoryTTF(
            const_cast<void*>(brandsResource->data),
            (int)brandsResource->size,
            32.0f,
            &iconConfig,
            iconRanges);
        if (brands) {
            Glass::SetIconFontBrands(brands);
        }
    }
}

void InitImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    SetupModernTheme();

    ImGui_ImplWin32_Init(g_hWnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    LoadLiquidGlassFonts();
}

bool BeginGlassPane(const char* id, ImVec2 size, Glass::Material material = Glass::Material::Thin) {
    const ImVec2 position = ImGui::GetCursorScreenPos();
    if (size.x <= 0.0f) {
        size.x = ImGui::GetContentRegionAvail().x;
    }
    if (size.y <= 0.0f) {
        size.y = ImGui::GetContentRegionAvail().y;
    }

    Glass::Primitive pane;
    pane.cx = position.x + size.x * 0.5f;
    pane.cy = position.y + size.y * 0.5f;
    pane.hw = size.x * 0.5f;
    pane.hh = size.y * 0.5f;
    pane.corner_radius = 16.0f;
    pane.fade = 0.92f;
    pane.material = material;
    g_glassRenderer.Submit(pane);

    return ImGui::BeginChild(id, size, false, ImGuiWindowFlags_NoBackground);
}

bool WindowControlButton(const char* id, Glass::Icon icon, ImVec2 position, const char* tooltip, bool destructive = false) {
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(position);
    ImGui::InvisibleButton("##window_control", ImVec2(kWindowControlSize, kWindowControlSize));

    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const bool clicked = ImGui::IsItemDeactivated() && hovered;
    if (ImGui::IsItemActivated()) {
        Glass::PlaySfx(Glass::Sfx::IconTap);
    }

    const float scale = active ? 0.94f : (hovered ? 1.04f : 1.0f);
    const ImVec2 center(
        position.x + kWindowControlSize * 0.5f,
        position.y + kWindowControlSize * 0.5f);

    Glass::Primitive surface;
    surface.cx = center.x;
    surface.cy = center.y;
    surface.hw = kWindowControlSize * 0.5f * scale;
    surface.hh = kWindowControlSize * 0.5f * scale;
    surface.corner_radius = 10.0f;
    surface.fade = hovered ? 1.0f : 0.72f;
    surface.material = hovered && !destructive ? Glass::Material::Accent : Glass::Material::Thin;
    g_glassRenderer.Submit(surface);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (hovered && destructive) {
        const ImU32 closeOverlay = active ? IM_COL32(255, 59, 48, 120) : IM_COL32(255, 59, 48, 92);
        drawList->AddRectFilled(
            position,
            ImVec2(position.x + kWindowControlSize, position.y + kWindowControlSize),
            closeOverlay,
            10.0f);
    }

    const ImU32 iconColor = hovered
        ? IM_COL32(255, 255, 255, 255)
        : IM_COL32(236, 237, 243, 218);
    const float iconSize = icon == Glass::Icon::Minus ? 13.0f : 14.0f;
    Glass::DrawIcon(drawList, icon, center, iconSize, iconColor, 2.0f);

    if (tooltip && *tooltip) {
        Glass::Tooltip(tooltip);
    }

    ImGui::PopID();
    return clicked;
}

void DrawWindowControls(const ImVec2& origin, float innerWidth) {
    const float groupWidth = (kWindowControlSize * 3.0f) + (kWindowControlGap * 2.0f);
    const float x = origin.x + innerWidth - groupWidth;
    const float y = origin.y - 4.0f;

    if (WindowControlButton("minimize", Glass::Icon::Minus, ImVec2(x, y), "Minimize")) {
        ShowWindow(g_hWnd, SW_MINIMIZE);
    }

    const bool maximized = IsZoomed(g_hWnd) != FALSE;
    const Glass::Icon maximizeIcon = maximized ? Glass::Icon::Compress : Glass::Icon::Expand;
    const char* maximizeTooltip = maximized ? "Restore" : "Maximize";
    if (WindowControlButton("maximize", maximizeIcon,
            ImVec2(x + kWindowControlSize + kWindowControlGap, y),
            maximizeTooltip)) {
        ShowWindow(g_hWnd, maximized ? SW_RESTORE : SW_MAXIMIZE);
    }

    if (WindowControlButton("close", Glass::Icon::Xmark,
            ImVec2(x + (kWindowControlSize + kWindowControlGap) * 2.0f, y),
            "Close",
            true)) {
        PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
    }
}

void RenderGUI() {
    RefreshCarSearch();
    SetWindowTextW(g_hWnd, kWindowTitle);

    Glass::SetAccent(0.04f, 0.50f, 1.0f);
    Glass::SetGlobalMaterial(0.55f, 0.38f, 1.35f, 24.0f, 2.5f, 0.22f, 0.10f);
    Glass::SetLiquidFlow(0.0f);

    ImGuiIO& io = ImGui::GetIO();
    TickFovOverride(io.DeltaTime);
    const ImVec2 display = io.DisplaySize;
    const ImVec2 cardSize(display.x - 72.0f, display.y - 72.0f);
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (Glass::BeginCard("FH6 Car Tool", Glass::Material::Regular, cardSize)) {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float innerWidth = ImGui::GetContentRegionAvail().x;
        const float innerHeight = ImGui::GetContentRegionAvail().y;
        const float sidebarWidth = 206.0f;
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        Glass::Primitive badge;
        badge.cx = origin.x + 20.0f;
        badge.cy = origin.y + 20.0f;
        badge.hw = 20.0f;
        badge.hh = 20.0f;
        badge.corner_radius = 12.0f;
        badge.fade = 1.0f;
        badge.material = Glass::Material::Accent;
        g_glassRenderer.Submit(badge);
        Glass::DrawIcon(drawList, Glass::Icon::Car, ImVec2(badge.cx, badge.cy), 20.0f, IM_COL32(255, 255, 255, 255));
        if (g_fontSemibold) {
            drawList->AddText(g_fontSemibold, 22.0f, ImVec2(origin.x + 52.0f, origin.y + 8.0f), IM_COL32(236, 237, 243, 255), "FH6 Tool");
        } else {
            drawList->AddText(ImVec2(origin.x + 52.0f, origin.y + 10.0f), IM_COL32(236, 237, 243, 255), "FH6 Tool");
        }

        static const Glass::Icon navIcons[] = {
            Glass::Icon::Plus,
            Glass::Icon::Bus,
            Glass::Icon::Car,
            Glass::Icon::Database
        };
        static const char* navLabels[] = {
            "Add Car",
            "Traffic Vehicles",
            "My Garage",
            "Misc"
        };
        static const ImU32 navTints[] = {
            IM_COL32(10, 132, 255, 255),
            IM_COL32(255, 149, 0, 255),
            IM_COL32(52, 199, 89, 255),
            IM_COL32(94, 92, 230, 255)
        };
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + 68.0f));
        const int navResult = Glass::SidebarNav("fh6_nav", navIcons, navLabels, navTints, 4, g_appState.activePage, sidebarWidth - 10.0f, 50.0f);
        if (navResult >= 0) {
            g_appState.activePage = navResult;
        }

        const float separatorX = origin.x + sidebarWidth + 8.0f;
        drawList->AddLine(ImVec2(separatorX, origin.y + 4.0f), ImVec2(separatorX, origin.y + innerHeight - 4.0f), IM_COL32(0, 0, 0, 28), 1.0f);
        drawList->AddLine(ImVec2(separatorX + 1.0f, origin.y + 4.0f), ImVec2(separatorX + 1.0f, origin.y + innerHeight - 4.0f), IM_COL32(255, 255, 255, 40), 1.0f);

        const float contentX = origin.x + sidebarWidth + 28.0f;
        const float contentWidth = innerWidth - sidebarWidth - 28.0f;
        static const char* pageTitles[] = { "Add Car", "Traffic Vehicles", "My Garage", "Misc" };
        static const char* pageSubtitles[] = {
            "Search and add regular vehicles",
            "Find, keep, or temporarily borrow traffic vehicles",
            "Inspect and manage the live garage",
            "FOV and live database changes"
        };
        ImGui::SetCursorScreenPos(ImVec2(contentX, origin.y));
        if (g_fontSemibold) {
            ImGui::PushFont(g_fontSemibold);
        }
        ImGui::TextUnformatted(pageTitles[g_appState.activePage]);
        if (g_fontSemibold) {
            ImGui::PopFont();
        }
        ImGui::SetCursorScreenPos(ImVec2(contentX, origin.y + 28.0f));
        ImGui::TextDisabled("%s", pageSubtitles[g_appState.activePage]);

        const float windowControlsWidth = (kWindowControlSize * 3.0f) + (kWindowControlGap * 2.0f);
        const float checkButtonSize = 38.0f;
        const float checkButtonX = origin.x + innerWidth - windowControlsWidth - checkButtonSize - 14.0f;
        ImGui::SetCursorScreenPos(ImVec2(checkButtonX, origin.y - 2.0f));
        if (Glass::IconButton("check_game", Glass::Icon::Bolt, checkButtonSize)) {
            CheckGameProcess();
        }
        if (ImGui::IsItemHovered()) {
            Glass::Tooltip("Check FH6 process");
        }
        DrawWindowControls(origin, innerWidth);

        ImGui::SetCursorScreenPos(ImVec2(origin.x + 4.0f, origin.y + innerHeight - 56.0f));
        Glass::StatusPill(g_appState.processAttached ? "FH6 running" : "FH6 unchecked",
            g_appState.processAttached ? IM_COL32(52, 209, 88, 255) : IM_COL32(142, 142, 147, 255));

        ImGui::SetCursorScreenPos(ImVec2(contentX, origin.y + 62.0f));
        ImGui::BeginChild("fh6_content", ImVec2(contentWidth, innerHeight - 62.0f), false, ImGuiWindowFlags_NoBackground);

        if (!g_appState.statusMessage.empty() && g_appState.statusTimer > 0) {
            ImVec4 statusColor = ImVec4(0.80f, 0.82f, 0.88f, 1.0f);
            if (g_statusKind == StatusKind::Success) {
                statusColor = ImVec4(0.35f, 0.95f, 0.55f, 1.0f);
            } else if (g_statusKind == StatusKind::Error) {
                statusColor = ImVec4(1.00f, 0.36f, 0.36f, 1.0f);
            }
            ImGui::TextColored(statusColor, "%s", g_appState.statusMessage.c_str());
            g_appState.statusTimer -= io.DeltaTime;
            ImGui::Separator();
        }

        if (g_appState.activePage == 0) {
            ImGui::TextColored(ImVec4(0.60f, 0.80f, 1.00f, 1.0f), "Cars");
            Glass::SearchField(g_appState.searchBuffer, 256, "Search cars, IDs, aliases, truck, suv, van, FE, WTAC...", ImGui::GetContentRegionAvail().x - 92.0f);
            ImGui::SameLine();
            if (ImGui::Button("Clear", ImVec2(84.0f, 0.0f))) {
                g_appState.searchBuffer[0] = '\0';
                g_appState.lastSearchQuery.clear();
                g_appState.selectedCarIndex = -1;
                ClearDuplicateAddPrompt();
                RefreshCarSearch();
            }
            ImGui::TextDisabled("%zu results", g_appState.filteredCars.size());
            if (g_appState.temporaryGarageRowId > 0) {
                ImGui::TextColored(ImVec4(0.90f, 0.66f, 0.25f, 1.0f),
                    "Temporary: %s (row %d)",
                    g_appState.temporaryCarName.c_str(),
                    g_appState.temporaryGarageRowId);
                ImGui::SameLine();
                if (ImGui::Button("Remove Temporary Row", ImVec2(168.0f, 0.0f))) {
                    RemoveTemporaryBorrowedCar();
                }
            }
            if (g_appState.borrowedGarageRowId > 0) {
                ImGui::TextColored(ImVec4(0.90f, 0.66f, 0.25f, 1.0f),
                    "Replaced: %s over %s (row %d)",
                    g_appState.borrowedCarName.c_str(),
                    g_appState.borrowedOriginalCarName.c_str(),
                    g_appState.borrowedGarageRowId);
                ImGui::SameLine();
                if (ImGui::Button("Restore Replaced Row##add", ImVec2(190.0f, 0.0f))) {
                    RestoreBorrowedGarageRowFromGame();
                }
            }

            float detailsWidth = 290.0f;
            float listWidth = ImGui::GetContentRegionAvail().x - detailsWidth - 10.0f;
            if (listWidth < 320.0f) {
                listWidth = ImGui::GetContentRegionAvail().x;
                detailsWidth = 0.0f;
            }

            float tabContentHeight = ImGui::GetContentRegionAvail().y - 8.0f;
            if (BeginGlassPane("CarList", ImVec2(listWidth, tabContentHeight))) {
                const auto& cars = g_appState.filteredCars;
                for (size_t i = 0; i < cars.size(); i++) {
                    ImGui::PushID((int)i);
                    bool isSelected = (g_appState.selectedCarIndex == (int)i);
                    std::string label = cars[i].displayName + "##" + std::to_string(cars[i].id);
                    if (ImGui::Selectable(label.c_str(), isSelected)) {
                        if (g_appState.selectedCarIndex != (int)i) {
                            ClearDuplicateAddPrompt();
                        }
                        g_appState.selectedCarIndex = (int)i;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::Text("ID: %d", cars[i].id);
                        ImGui::Text("Media: %s", cars[i].name.c_str());
                        ImGui::Text("PI: %d", cars[i].pi);
                        ImGui::Text("Availability: %s", cars[i].notAvailableInAutoshow ? "Offsale / hidden" : "Autoshow");
                        ImGui::Text("Cost: %d CR", cars[i].baseCost);
                        ImGui::EndTooltip();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();

            if (detailsWidth > 0.0f) {
                ImGui::SameLine();
                BeginGlassPane("SelectedCar", ImVec2(detailsWidth, tabContentHeight), Glass::Material::Regular);
            }

            if (g_appState.selectedCarIndex >= 0 && !g_appState.filteredCars.empty()) {
                const auto& cars = g_appState.filteredCars;
                if (g_appState.selectedCarIndex < (int)cars.size()) {
                    const Car& car = cars[g_appState.selectedCarIndex];
                    ImGui::TextWrapped("%s", car.displayName.c_str());
                    ImGui::Separator();
                    ImGui::Text("ID: %d", car.id);
                    ImGui::Text("Year: %d", car.year);
                    ImGui::Text("Make: %s", car.make.c_str());
                    ImGui::TextWrapped("Model: %s", car.model.c_str());
                    ImGui::TextWrapped("Media: %s", car.name.c_str());
                    ImGui::Text("PI: %d", car.pi);
                    ImGui::Text("Cost: %d CR", car.baseCost);
                    ImGui::Text("Autoshow: %s", car.notAvailableInAutoshow ? "No" : "Yes");
                    if (car.visibleOnlyIfOwned) {
                        ImGui::TextColored(ImVec4(0.90f, 0.66f, 0.25f, 1.0f), "Visible only when owned");
                    }
                    if (!car.isDrivable) {
                        ImGui::TextColored(ImVec4(0.90f, 0.66f, 0.25f, 1.0f), "Non-drivable database row");
                    }

                    ImGui::Spacing();
                    if (Glass::Button("Add Car", true, ImVec2(ImGui::GetContentRegionAvail().x, 40.0f))) {
                        auto result = GrantCarToGarage(car.id, false);
                        if (result.alreadyOwned) {
                            g_appState.showDuplicateAddConfirm = true;
                            g_appState.pendingDuplicateCarId = car.id;
                            g_appState.pendingDuplicateCount = result.ownedCount;
                            SetStatus("Already owned. Confirm below if you want another copy.", StatusKind::Info, 5.0f);
                        } else if (result.success) {
                            ClearDuplicateAddPrompt();
                            SetStatus(result.message + " Reopen your garage if it does not appear immediately.", StatusKind::Success, 5.0f);
                            g_appState.garageLoaded = false;
                        } else {
                            SetStatus("Add failed: " + result.message, StatusKind::Error, 5.0f);
                        }
                    }

                    if (Glass::Button("Temp Borrow Current Car", false, ImVec2(ImGui::GetContentRegionAvail().x, 40.0f))) {
                        SetTemporaryCurrentCar(car);
                    }

                    if (g_appState.showDuplicateAddConfirm && g_appState.pendingDuplicateCarId == car.id) {
                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();
                        const char* copyWord = g_appState.pendingDuplicateCount == 1 ? "copy" : "copies";
                        ImGui::TextColored(ImVec4(0.90f, 0.66f, 0.25f, 1.0f),
                            "You already own %d %s.", g_appState.pendingDuplicateCount, copyWord);
                        ImGui::TextWrapped("Adding this car again will create a duplicate garage entry.");
                        ImGui::Spacing();

                        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.62f, 0.42f, 0.10f, 1.00f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.78f, 0.54f, 0.14f, 1.00f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.92f, 0.66f, 0.18f, 1.00f));
                        if (ImGui::Button("Add Duplicate", ImVec2(-1.0f, 0.0f))) {
                            auto result = GrantCarToGarage(car.id, true);
                            if (result.success) {
                                g_appState.pendingDuplicateCount = result.ownedCount;
                                SetStatus(result.message + " You now own " + std::to_string(result.ownedCount) + " copies.", StatusKind::Success, 5.0f);
                                g_appState.garageLoaded = false;
                            } else {
                                SetStatus("Duplicate add failed: " + result.message, StatusKind::Error, 5.0f);
                            }
                        }
                        ImGui::PopStyleColor(3);

                        if (ImGui::Button("Cancel", ImVec2(-1.0f, 0.0f))) {
                            ClearDuplicateAddPrompt();
                            SetStatus("Duplicate add cancelled.", StatusKind::Info, 2.0f);
                        }
                    }
                }
            } else {
                ImGui::TextDisabled("Select a car to view details.");
            }

            if (detailsWidth > 0.0f) {
                ImGui::EndChild();
            }

        }

        if (g_appState.activePage == 1) {
            ImGui::TextColored(ImVec4(0.60f, 0.80f, 1.00f, 1.0f), "Traffic Vehicles");
            Glass::SearchField(g_appState.trafficSearchBuffer, 256, "Search traffic vehicles, trucks, buses, taxis...", ImGui::GetContentRegionAvail().x - 92.0f);
            ImGui::SameLine();
            if (ImGui::Button("Clear##traffic", ImVec2(84.0f, 0.0f))) {
                g_appState.trafficSearchBuffer[0] = '\0';
                g_appState.trafficSelectedIndex = -1;
            }

            std::vector<Car> trafficCars = GetFilteredTrafficVehicles(g_appState.trafficSearchBuffer);
            if (g_appState.trafficSelectedIndex >= (int)trafficCars.size()) {
                g_appState.trafficSelectedIndex = -1;
            }

            ImGui::TextDisabled("%zu traffic results", trafficCars.size());
            if (g_appState.temporaryGarageRowId > 0) {
                ImGui::TextColored(ImVec4(0.90f, 0.66f, 0.25f, 1.0f),
                    "Temporary: %s (row %d)",
                    g_appState.temporaryCarName.c_str(),
                    g_appState.temporaryGarageRowId);
                ImGui::SameLine();
                if (ImGui::Button("Remove Temporary Row##traffic", ImVec2(184.0f, 0.0f))) {
                    RemoveTemporaryBorrowedCar();
                }
            }
            if (g_appState.borrowedGarageRowId > 0) {
                ImGui::TextColored(ImVec4(0.90f, 0.66f, 0.25f, 1.0f),
                    "Replaced: %s over %s (row %d)",
                    g_appState.borrowedCarName.c_str(),
                    g_appState.borrowedOriginalCarName.c_str(),
                    g_appState.borrowedGarageRowId);
                ImGui::SameLine();
                if (ImGui::Button("Restore Replaced Row##traffic", ImVec2(190.0f, 0.0f))) {
                    RestoreBorrowedGarageRowFromGame();
                }
            }

            float detailsWidth = 300.0f;
            float listWidth = ImGui::GetContentRegionAvail().x - detailsWidth - 10.0f;
            if (listWidth < 320.0f) {
                listWidth = ImGui::GetContentRegionAvail().x;
                detailsWidth = 0.0f;
            }

            float tabContentHeight = ImGui::GetContentRegionAvail().y - 8.0f;
            if (BeginGlassPane("TrafficList", ImVec2(listWidth, tabContentHeight))) {
                for (size_t i = 0; i < trafficCars.size(); i++) {
                    ImGui::PushID((int)i);
                    bool isSelected = (g_appState.trafficSelectedIndex == (int)i);
                    std::string label = trafficCars[i].displayName + "##traffic" + std::to_string(trafficCars[i].id);
                    if (ImGui::Selectable(label.c_str(), isSelected)) {
                        g_appState.trafficSelectedIndex = (int)i;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::Text("ID: %d", trafficCars[i].id);
                        ImGui::Text("Media: %s", trafficCars[i].name.c_str());
                        ImGui::Text("PI: %d", trafficCars[i].pi);
                        ImGui::Text("Drivable flag: %s", trafficCars[i].isDrivable ? "Yes" : "No");
                        ImGui::EndTooltip();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();

            if (detailsWidth > 0.0f) {
                ImGui::SameLine();
                BeginGlassPane("TrafficDetail", ImVec2(detailsWidth, tabContentHeight), Glass::Material::Regular);
            }

            if (g_appState.trafficSelectedIndex >= 0 &&
                g_appState.trafficSelectedIndex < (int)trafficCars.size()) {
                const Car& car = trafficCars[g_appState.trafficSelectedIndex];
                ImGui::TextWrapped("%s", car.displayName.c_str());
                ImGui::Separator();
                ImGui::Text("ID: %d", car.id);
                ImGui::Text("Year: %d", car.year);
                ImGui::Text("Make: %s", car.make.c_str());
                ImGui::TextWrapped("Model: %s", car.model.c_str());
                ImGui::TextWrapped("Media: %s", car.name.c_str());
                ImGui::Text("PI: %d", car.pi);
                ImGui::Text("Autoshow: %s", car.notAvailableInAutoshow ? "No" : "Yes");
                ImGui::Text("Drivable flag: %s", car.isDrivable ? "Yes" : "No");
                if (car.visibleOnlyIfOwned) {
                    ImGui::TextColored(ImVec4(0.90f, 0.66f, 0.25f, 1.0f), "Visible only when owned");
                }
                ImGui::Spacing();
                ImGui::TextWrapped("Add keeps it in garage. Temp Borrow targets the current player car.");
                ImGui::Spacing();

                ImGui::BeginDisabled(!car.isDrivable);
                if (Glass::Button("Add Traffic Row", true, ImVec2(ImGui::GetContentRegionAvail().x, 40.0f))) {
                    auto result = GrantCarToGarage(car.id, false, true);
                    if (result.alreadyOwned) {
                        SetStatus("Traffic car is already in the garage database; visibility prep was refreshed.", StatusKind::Info, 7.0f);
                        g_appState.garageLoaded = false;
                    } else if (result.success) {
                        SetStatus("Prepared traffic car was added and unhidden for FH garage. Reopen garage/car select if it does not refresh.", StatusKind::Success, 8.0f);
                        g_appState.garageLoaded = false;
                    } else {
                        SetStatus("Traffic add failed: " + result.message, StatusKind::Error, 6.0f);
                    }
                }

                if (Glass::Button("Temp Borrow Current Car", false, ImVec2(ImGui::GetContentRegionAvail().x, 40.0f))) {
                    SetTemporaryCurrentCar(car);
                }
                ImGui::EndDisabled();

                const bool hasBorrowTarget =
                    g_appState.garageLoaded &&
                    g_appState.garageSelectedIndex >= 0 &&
                    g_appState.garageSelectedIndex < (int)g_appState.garageCars.size();
                ImGui::BeginDisabled(!car.isDrivable || !hasBorrowTarget || g_appState.borrowedGarageRowId > 0);
                if (Glass::Button("Replace Selected Garage Row", false, ImVec2(ImGui::GetContentRegionAvail().x, 40.0f))) {
                    BorrowOverSelectedGarageRow(car);
                }
                ImGui::EndDisabled();

                if (!hasBorrowTarget) {
                    ImGui::TextDisabled("For replace: refresh My Garage and select the row to replace.");
                } else if (g_appState.borrowedGarageRowId > 0) {
                    ImGui::TextDisabled("Restore the replaced row before replacing another car.");
                }

                if (!car.isDrivable) {
                    ImGui::TextDisabled("Non-drivable traffic rows cannot be added safely.");
                }
            } else {
                ImGui::TextDisabled("Select a traffic vehicle to view details.");
            }

            if (detailsWidth > 0.0f) {
                ImGui::EndChild();
            }

        }

        if (g_appState.activePage == 2) {
            if (Glass::Button("Refresh Library", true, ImVec2(150.0f, 40.0f))) {
                RefreshGarageLibraryFromGame();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%zu cars in garage", g_appState.garageCars.size());

            Glass::SearchField(g_appState.garageSearchBuffer, sizeof(g_appState.garageSearchBuffer),
                "Filter your garage...", ImGui::GetContentRegionAvail().x - 10.0f);

            ImGui::Spacing();

            if (!g_appState.garageLoaded && g_appState.garageErrorMessage.empty()) {
                ImGui::TextColored(ImVec4(0.60f, 0.70f, 0.80f, 1.0f),
                    "Press  \"Refresh Library\"  to load your current garage.");
            } else if (!g_appState.garageErrorMessage.empty()) {
                ImGui::TextColored(ImVec4(1.00f, 0.36f, 0.36f, 1.0f),
                    "Error: %s", g_appState.garageErrorMessage.c_str());
            } else {
                std::string garageFilter(g_appState.garageSearchBuffer);
                std::string lowerFilter;
                lowerFilter.reserve(garageFilter.size());
                for (unsigned char ch : garageFilter)
                    lowerFilter.push_back((char)std::tolower(ch));

                float detailsW = 270.0f;
                float listW    = ImGui::GetContentRegionAvail().x - detailsW - 10.0f;
                if (listW < 280.0f) { listW = ImGui::GetContentRegionAvail().x; detailsW = 0.0f; }
                float panelH   = ImGui::GetContentRegionAvail().y - 8.0f;

                if (BeginGlassPane("GarageList", ImVec2(listW, panelH))) {
                    for (size_t i = 0; i < g_appState.garageCars.size(); i++) {
                        const auto& gc = g_appState.garageCars[i];

                        if (!lowerFilter.empty()) {
                            std::string lowerName;
                            lowerName.reserve(gc.displayName.size());
                            for (unsigned char ch : gc.displayName)
                                lowerName.push_back((char)std::tolower(ch));
                            if (lowerName.find(lowerFilter) == std::string::npos &&
                                std::to_string(gc.carId).find(lowerFilter) == std::string::npos)
                                continue;
                        }

                        ImGui::PushID((int)i);
                        bool sel = (g_appState.garageSelectedIndex == (int)i);
                        std::string label = gc.displayName;
                        if (gc.garageId == g_appState.temporaryGarageRowId) {
                            label += " [temporary]";
                        }
                        if (gc.garageId == g_appState.borrowedGarageRowId) {
                            label += " [replaced]";
                        }
                        label += "##g" + std::to_string(i);
                        if (ImGui::Selectable(label.c_str(), sel)) {
                            g_appState.garageSelectedIndex = (int)i;
                            g_appState.showRemoveConfirm = false;
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::BeginTooltip();
                            ImGui::Text("Car ID: %d", gc.carId);
                            ImGui::Text("Garage row ID: %d", gc.garageId);
                            ImGui::EndTooltip();
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndChild();

                if (detailsW > 0.0f) {
                    ImGui::SameLine();
                    BeginGlassPane("GarageDetail", ImVec2(detailsW, panelH), Glass::Material::Regular);
                }

                if (g_appState.garageSelectedIndex >= 0 &&
                    g_appState.garageSelectedIndex < (int)g_appState.garageCars.size()) {

                    const auto& gc = g_appState.garageCars[g_appState.garageSelectedIndex];
                    ImGui::TextWrapped("%s", gc.displayName.c_str());
                    if (gc.garageId == g_appState.temporaryGarageRowId) {
                        ImGui::TextColored(ImVec4(0.90f, 0.66f, 0.25f, 1.0f), "Temporary garage row");
                    }
                    if (gc.garageId == g_appState.borrowedGarageRowId) {
                        ImGui::TextColored(ImVec4(0.90f, 0.66f, 0.25f, 1.0f),
                            "Replaced over: %s", g_appState.borrowedOriginalCarName.c_str());
                    }
                    ImGui::Separator();
                    ImGui::Text("Car ID: %d", gc.carId);
                    ImGui::Text("Garage Row ID: %d", gc.garageId);

                    Car* dbCar = CarDatabase::GetInstance().GetCarById(gc.carId);
                    if (dbCar) {
                        ImGui::Text("Year: %d", dbCar->year);
                        ImGui::Text("Make: %s", dbCar->make.c_str());
                        ImGui::TextWrapped("Model: %s", dbCar->model.c_str());
                        ImGui::Text("PI: %d", dbCar->pi);
                        ImGui::Text("Cost: %d CR", dbCar->baseCost);
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    if (gc.garageId == g_appState.borrowedGarageRowId) {
                        if (ImGui::Button("Restore Replaced Row", ImVec2(-1.0f, 0.0f))) {
                            RestoreBorrowedGarageRowFromGame();
                        }
                        ImGui::Spacing();
                    }

                    if (!g_appState.showRemoveConfirm) {
                        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.10f, 0.10f, 1.00f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.75f, 0.15f, 0.15f, 1.00f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.90f, 0.20f, 0.20f, 1.00f));
                        if (ImGui::Button("Remove from Garage", ImVec2(-1.0f, 0.0f))) {
                            g_appState.showRemoveConfirm = true;
                        }
                        ImGui::PopStyleColor(3);
                    } else {
                        ImGui::TextColored(ImVec4(1.00f, 0.80f, 0.20f, 1.0f),
                            "Remove  \"%s\"  from your garage?", gc.displayName.c_str());
                        ImGui::Spacing();

                        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.10f, 0.10f, 1.00f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.75f, 0.15f, 0.15f, 1.00f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.90f, 0.20f, 0.20f, 1.00f));
                        if (ImGui::Button("Yes, Remove", ImVec2(-1.0f, 0.0f))) {
                            const int removedGarageId = gc.garageId;
                            auto res = RemoveGarageRowById(gc.garageId);
                            if (res.success) {
                                SetStatus("Removed " + gc.displayName + " garage row.", StatusKind::Success, 4.0f);
                                if (removedGarageId == g_appState.temporaryGarageRowId) {
                                    ClearTemporaryBorrowState();
                                }
                                if (removedGarageId == g_appState.borrowedGarageRowId) {
                                    ClearBorrowedGarageState();
                                }
                                RefreshGarageLibraryFromGame();
                            } else {
                                SetStatus("Remove failed: " + res.message, StatusKind::Error, 5.0f);
                                g_appState.showRemoveConfirm = false;
                            }
                        }
                        ImGui::PopStyleColor(3);

                        ImGui::Spacing();
                        if (ImGui::Button("Cancel", ImVec2(-1.0f, 0.0f))) {
                            g_appState.showRemoveConfirm = false;
                        }
                    }
                } else {
                    ImGui::TextDisabled("Select a car to view details.");
                }

                if (detailsW > 0.0f) {
                    ImGui::EndChild();
                }
            }

        }

        if (g_appState.activePage == 3) {
            ImGui::TextColored(ImVec4(0.60f, 0.80f, 1.00f, 1.0f), "Misc Tools");
            ImGui::TextWrapped("FOV applies to live camera memory. Database mods change FH6's live database and create backup tables before Autoshow and car price edits.");
            ImGui::Spacing();

            ImGui::TextColored(ImVec4(0.60f, 0.80f, 1.00f, 1.0f), "FOV Override");
            const float fovSliderWidth = (std::clamp)(ImGui::GetContentRegionAvail().x - 96.0f, 120.0f, 360.0f);
            ImGui::SetNextItemWidth(fovSliderWidth);
            ImGui::SliderFloat("##misc_fov", &g_appState.fovValue, 60.0f, 200.0f, "%.0f");
            ImGui::SameLine();
            if (ImGui::Button("Default", ImVec2(86.0f, 0.0f))) {
                g_appState.fovValue = 90.0f;
                if (g_appState.fovKeepApplied) {
                    ApplyFovFromUi(false);
                }
            }

            if (ImGui::Button("Apply FOV", ImVec2(140.0f, 0.0f))) {
                ApplyFovFromUi(true);
            }
            ImGui::SameLine();
            bool keepApplied = g_appState.fovKeepApplied;
            if (ImGui::Checkbox("Keep Applied", &keepApplied)) {
                g_appState.fovKeepApplied = keepApplied;
                if (g_appState.fovKeepApplied) {
                    ApplyFovFromUi(g_appState.fovLastVerified <= 0);
                } else {
                    ClearFovOverrideTargets();
                    g_appState.fovLastVerified = 0;
                    SetStatus("FOV keep-applied disabled.", StatusKind::Info, 3.0f);
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Verified: %d", g_appState.fovLastVerified);
            ImGui::TextDisabled("Load into gameplay first. If it misses, switch camera once and apply again.");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGui::Button("All Cars in Autoshow", ImVec2(210.0f, 0.0f))) {
                QueueMiscAction(PendingMiscAction::AllCarsAutoshow);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Hidden database cars");

            if (ImGui::Button("Free Cars", ImVec2(210.0f, 0.0f))) {
                QueueMiscAction(PendingMiscAction::FreeCars);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Car prices to 0 CR");

            if (ImGui::Button("Free Upgrades", ImVec2(210.0f, 0.0f))) {
                QueueMiscAction(PendingMiscAction::FreeUpgrades);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Parts and presets");

            if (ImGui::Button("Clear New Tags", ImVec2(210.0f, 0.0f))) {
                QueueMiscAction(PendingMiscAction::ClearNewTags);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Garage viewed flags");

            if (g_appState.pendingMiscAction != PendingMiscAction::None) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                const char* title = MiscActionTitle(g_appState.pendingMiscAction);
                ImGui::TextColored(ImVec4(0.90f, 0.66f, 0.25f, 1.0f), "Confirm: %s", title);
                ImGui::TextWrapped("%s", MiscActionDescription(g_appState.pendingMiscAction));
                ImGui::Spacing();

                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.62f, 0.42f, 0.10f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.78f, 0.54f, 0.14f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.92f, 0.66f, 0.18f, 1.00f));
                if (ImGui::Button("Confirm", ImVec2(120.0f, 0.0f))) {
                    ExecuteQueuedMiscAction();
                }
                ImGui::PopStyleColor(3);

                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) {
                    g_appState.pendingMiscAction = PendingMiscAction::None;
                    SetStatus("Misc action cancelled.", StatusKind::Info, 2.0f);
                }
            }

        }

        ImGui::EndChild();
    }
    Glass::EndCard();

    static bool dragging = false;
    static POINT dragCursorStart = {};
    static RECT dragWindowStart = {};
    const bool overItem = ImGui::IsAnyItemHovered() || ImGui::IsAnyItemActive();
    const bool popupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (!dragging && io.MouseClicked[0] && !overItem && !popupOpen) {
        dragging = true;
        GetCursorPos(&dragCursorStart);
        GetWindowRect(g_hWnd, &dragWindowStart);
    }
    if (dragging) {
        if (!io.MouseDown[0]) {
            dragging = false;
        } else {
            POINT cursor = {};
            GetCursorPos(&cursor);
            SetWindowPos(g_hWnd, nullptr,
                dragWindowStart.left + cursor.x - dragCursorStart.x,
                dragWindowStart.top + cursor.y - dragCursorStart.y,
                0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
}
