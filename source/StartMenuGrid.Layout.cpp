// SPDX-License-Identifier: GPL-3.0-only
// The UI thread attachment was adapted from Start Menu Size by Michael Maltsev.
// https://github.com/ramensoftware/windhawk-mods/blob/main/mods/start-menu-size.wh.cpp
#include <windows.h>
#include <cstdarg>
#include <inspectable.h>
#include <roapi.h>
#include <winstring.h>
#include <winrt/base.h>
#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Data.h>
#include <winrt/Windows.UI.Xaml.Media.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <optional>
#include <vector>

using namespace winrt::Windows::UI::Xaml;
using XamlValue = winrt::Windows::Foundation::IInspectable;

namespace {

struct GridConfig {
    int columns = 0;
    int rows = 0;
    bool operator==(const GridConfig&) const = default;
};

extern GridConfig currentSettings;
void ReportLayout(int columns, int rows);

void Log(PCWSTR format, ...) {
    wchar_t message[320];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, ARRAYSIZE(message), _TRUNCATE, format, args);
    va_end(args);
    OutputDebugStringW(message);
}

constexpr double tileWidth = 96;
constexpr double tileHeight = 84;
constexpr double horizontalPadding = 66;
constexpr double verticalPadding = 390;
constexpr double windowBorder = 24;
constexpr double taskbarGap = 12;

// The ABI below was checked against Microsoft's public symbols for the
// StartMenu.dll shipped with build 22631.7517. Both interface identity and
// the vtable from that DLL must match before calling a private method.
constexpr GUID pinnedListId{0xce20b4f6, 0x5c9e, 0x5509, {0xb1, 0xe9, 0xf5, 0xfd, 0xb2, 0x30, 0xc6, 0xe2}};
constexpr GUID gridModelId{0x5b0748e2, 0xef06, 0x52af, {0x9d, 0x15, 0x3d, 0x83, 0x85, 0xae, 0x0f, 0x91}};
constexpr size_t pinnedListVtable = 0x2fd4d0;
constexpr size_t gridModelVtable = 0x302918;
constexpr int getViewModel = 6;
constexpr int getItemsPerPage = 9;
constexpr int setItemsPerPage = 10;
constexpr int getItemsPerRow = 11;
constexpr int setItemsPerRow = 12;

using UiCallback = void (*)();
std::atomic<UiCallback> uiCallback{};
std::mutex uiCallbackMutex;
FrameworkElement content{nullptr};
Window startWindow{nullptr};
winrt::event_token layoutToken{};
winrt::event_token visibilityToken{};
bool pending = true;
bool attached = false;

HWND FindStartWindow();

struct MenuRegion {
    HWND window = nullptr;
    int top = 0;
    int bottom = 0;
    int left = 0;
    int stockWidth = 0;
    int width = 0;
};

thread_local MenuRegion menuRegion;
auto setWindowRgnOriginal = &::SetWindowRgn;

int WINAPI SetStartWindowRegion(HWND window, HRGN region, BOOL redraw) {
    RECT bounds{};
    RECT windowBounds{};
    if (region && window == menuRegion.window && (menuRegion.width || menuRegion.bottom) &&
        GetRgnBox(region, &bounds) == SIMPLEREGION &&
        GetWindowRect(window, &windowBounds)) {
        int right = bounds.right;
        int bottom = bounds.bottom;
        const int regionWidth = bounds.right - bounds.left;
        // A page transition can submit the stock width with the custom left
        // edge. Expand that specific rectangle without changing its anchor.
        if (menuRegion.width && std::abs(bounds.left - menuRegion.left) <= 2 &&
            std::abs(regionWidth - menuRegion.stockWidth) <= 2) {
            right = std::max(right, std::min(static_cast<int>(bounds.left) + menuRegion.width,
                static_cast<int>(windowBounds.right - windowBounds.left)));
        }
        if (menuRegion.bottom && std::abs(bounds.top - menuRegion.top) <= 2) {
            bottom = std::max(bottom, std::min(menuRegion.bottom,
                static_cast<int>(windowBounds.bottom - windowBounds.top)));
        }
        if ((right != bounds.right || bottom != bounds.bottom) &&
            SetRectRgn(region, bounds.left, bounds.top, right, bottom)) {
            int result = setWindowRgnOriginal(window, region, redraw);
            if (!result) {
                SetRectRgn(region, bounds.left, bounds.top, bounds.right, bounds.bottom);
            }
            return result;
        }
    }
    return setWindowRgnOriginal(window, region, redraw);
}

void UpdateVerticalRegion(double top, double height) {
    HWND window = FindStartWindow();
    UINT dpi = window ? GetDpiForWindow(window) : 0;
    if (!dpi) {
        menuRegion.bottom = 0;
        return;
    }
    if (menuRegion.window != window) {
        menuRegion = {window};
    }
    const double scale = dpi / 96.0;
    const double shadow = windowBorder / 2;
    menuRegion.top = static_cast<int>(std::lround((top - shadow) * scale));
    menuRegion.bottom = static_cast<int>(std::lround((top + height + shadow) * scale));
}

const BYTE* GetSupportedStartMenu() {
    auto image = reinterpret_cast<const BYTE*>(GetModuleHandleW(L"StartMenu.dll"));
    if (!image) {
        return nullptr;
    }
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    return nt->FileHeader.TimeDateStamp == 0x68ee987e &&
        nt->OptionalHeader.SizeOfImage == 0x3f6000 ? image : nullptr;
}

winrt::com_ptr<::IInspectable> AsVerifiedInterface(winrt::Windows::Foundation::IUnknown const& object,
                                                 GUID const& iid, const BYTE* vtable) {
    winrt::com_ptr<::IInspectable> iface;
    if (!object) {
        return iface;
    }
    HRESULT result = reinterpret_cast<::IUnknown*>(winrt::get_abi(object))->QueryInterface(iid, iface.put_void());
    if (FAILED(result) || !iface || *reinterpret_cast<const BYTE**>(iface.get()) != vtable) {
        iface = nullptr;
    }
    return iface;
}

template<class Value>
void ReadProperty(::IInspectable* object, int slot, Value* output) {
    using Get = HRESULT (WINAPI*)(::IInspectable*, Value*);
    auto table = *reinterpret_cast<void***>(object);
    winrt::check_hresult(reinterpret_cast<Get>(table[slot])(object, output));
}

template<class Value>
void WriteProperty(::IInspectable* object, int slot, Value input) {
    using Put = HRESULT (WINAPI*)(::IInspectable*, Value);
    auto table = *reinterpret_cast<void***>(object);
    winrt::check_hresult(reinterpret_cast<Put>(table[slot])(object, input));
}

winrt::com_ptr<::IInspectable> GetGridModel(FrameworkElement const& list, const BYTE* image) {
    auto listApi = AsVerifiedInterface(list, pinnedListId, image + pinnedListVtable);
    if (!listApi) {
        return nullptr;
    }
    XamlValue model{nullptr};
    ReadProperty(listApi.get(), getViewModel, winrt::put_abi(model));
    return AsVerifiedInterface(model, gridModelId, image + gridModelVtable);
}

struct SavedProperty {
    FrameworkElement element;
    DependencyProperty property;
    XamlValue localValue;
    Data::Binding binding{nullptr};
};

struct RecommendationRow {
    winrt::weak_ref<Controls::ListViewItem> element;
    double stockWidth;
    double appliedWidth;
};

struct PinnedHeightOverride {
    FrameworkElement element;
    int64_t token = 0;
};

struct Layout {
    FrameworkElement frame;
    Controls::ItemsWrapGrid wrap;
    winrt::com_ptr<::IInspectable> model;
    double originalWidth;
    double originalHeight;
    unsigned originalColumns;
    int originalPageSlots;
    std::vector<SavedProperty> values;
    double pinnedHeight = 0;
    std::vector<PinnedHeightOverride> pinnedHeights;
    XamlValue nativeTop{nullptr};
    int64_t topToken = 0;
    double anchoredHeight = 0;
    XamlValue nativeLeft{nullptr};
    int64_t leftToken = 0;
    double anchoredWidth = 0;
    bool correctedLeft = false;
    double recommendationExtraWidth = 0;
    FrameworkElement recommendations{nullptr};
    std::vector<RecommendationRow> recommendationRows;

    void Set(FrameworkElement const& element, DependencyProperty const& property, XamlValue const& value) {
        auto saved = std::find_if(values.begin(), values.end(), [&](auto const& entry) {
            return entry.element == element && entry.property == property;
        });
        if (saved == values.end()) {
            SavedProperty entry{element, property, element.ReadLocalValue(property)};
            if (auto expression = element.GetBindingExpression(property)) {
                entry.binding = expression.ParentBinding();
            }
            values.push_back(std::move(entry));
        }
        element.SetValue(property, value);
    }

    void Set(FrameworkElement const& element, DependencyProperty const& property, double value) {
        Set(element, property, winrt::box_value(value));
    }

    void KeepPinnedHeight(FrameworkElement const& element) {
        auto property = FrameworkElement::HeightProperty();
        Set(element, property, pinnedHeight);
        if (std::any_of(pinnedHeights.begin(), pinnedHeights.end(), [&](auto const& entry) {
                return entry.element == element;
            })) {
            return;
        }

        // Start recalculates the pinned area when an app is pinned or unpinned.
        // Keep the configured row count through that update, before the next measure.
        auto& entry = pinnedHeights.emplace_back(PinnedHeightOverride{element});
        entry.token = element.RegisterPropertyChangedCallback(property,
            [this](DependencyObject const& sender, DependencyProperty const& changedProperty) {
                try {
                    auto element = sender.as<FrameworkElement>();
                    if (element.Height() == pinnedHeight) {
                        return;
                    }
                    auto saved = std::find_if(values.begin(), values.end(), [&](auto const& entry) {
                        return entry.element == element && entry.property == changedProperty;
                    });
                    if (saved != values.end() && !saved->binding) {
                        saved->localValue = element.ReadLocalValue(changedProperty);
                    }
                    element.Height(pinnedHeight);
                } catch (...) {
                    Log(L"Keeping the pinned grid height failed: 0x%08X", winrt::to_hresult());
                }
            });
    }

    void AnchorToTaskbar(double height) {
        if (height <= originalHeight) {
            RestoreAnchor();
            return;
        }
        auto topProperty = Controls::Canvas::TopProperty();
        anchoredHeight = height;
        if (!topToken) {
            nativeTop = frame.ReadLocalValue(topProperty);
            topToken = frame.RegisterPropertyChangedCallback(topProperty,
                [this](DependencyObject const&, DependencyProperty const& property) {
                    try {
                        const double top = content.ActualHeight() - anchoredHeight - taskbarGap;
                        if (Controls::Canvas::GetTop(frame) != top) {
                            nativeTop = frame.ReadLocalValue(property);
                            if (top >= taskbarGap) {
                                UpdateVerticalRegion(top, anchoredHeight);
                                Controls::Canvas::SetTop(frame, top);
                            } else {
                                menuRegion.bottom = 0;
                            }
                        }
                    } catch (...) {
                        Log(L"Keeping the Start menu anchored failed: 0x%08X", winrt::to_hresult());
                    }
                });
        }
        // StartDocked reads Canvas.Top for both its HWND region and XAML layout.
        // It can also rewrite this property when opening or arranging the menu.
        const double top = content.ActualHeight() - height - taskbarGap;
        UpdateVerticalRegion(top, height);
        Controls::Canvas::SetTop(frame, top);
    }

    void RestoreAnchor() {
        menuRegion.bottom = 0;
        if (!topToken) {
            return;
        }
        auto property = Controls::Canvas::TopProperty();
        frame.UnregisterPropertyChangedCallback(property, topToken);
        topToken = 0;
        if (!nativeTop || nativeTop == DependencyProperty::UnsetValue()) {
            frame.ClearValue(property);
        } else {
            frame.SetValue(property, nativeTop);
        }
        nativeTop = nullptr;
    }

    void AnchorHorizontalCenter(double width) {
        if (width - originalWidth <= 4) {
            RestoreHorizontalCenter();
            return;
        }
        auto property = Controls::Canvas::LeftProperty();
        anchoredWidth = width;
        if (!leftToken) {
            leftToken = frame.RegisterPropertyChangedCallback(property,
                [this](DependencyObject const&, DependencyProperty const& changed) {
                    try {
                        CorrectStockCenter(changed);
                        UpdateHorizontalRegion();
                    } catch (...) {
                        Log(L"Keeping the Start menu centered failed: 0x%08X", winrt::to_hresult());
                    }
                });
        }
        CorrectStockCenter(property);
        UpdateHorizontalRegion();
    }

    void UpdateHorizontalRegion() {
        HWND window = FindStartWindow();
        UINT dpi = window ? GetDpiForWindow(window) : 0;
        double left = Controls::Canvas::GetLeft(frame);
        if (!dpi || !std::isfinite(left)) {
            menuRegion.width = 0;
            return;
        }
        if (menuRegion.window != window) {
            menuRegion = {window};
        }
        const double scale = dpi / 96.0;
        menuRegion.left = static_cast<int>(std::lround((left - windowBorder / 2) * scale));
        menuRegion.stockWidth = static_cast<int>(std::lround((originalWidth + windowBorder) * scale));
        menuRegion.width = static_cast<int>(std::lround((anchoredWidth + windowBorder) * scale));
    }

    void CorrectStockCenter(DependencyProperty const& property) {
        const double left = Controls::Canvas::GetLeft(frame);
        const double stockCenter = (content.ActualWidth() - originalWidth) / 2;
        // Returning from Recommended > More can write the stock-width position
        // for one frame. Correct only that position so left alignment stays native.
        if (std::abs(left - stockCenter) <= 1) {
            nativeLeft = frame.ReadLocalValue(property);
            correctedLeft = true;
            Controls::Canvas::SetLeft(frame, left - (anchoredWidth - originalWidth) / 2);
        }
    }

    void RestoreHorizontalCenter() {
        menuRegion.width = 0;
        if (!leftToken) {
            return;
        }
        auto property = Controls::Canvas::LeftProperty();
        frame.UnregisterPropertyChangedCallback(property, leftToken);
        leftToken = 0;
        const double centeredLeft = (content.ActualWidth() - anchoredWidth) / 2;
        if (correctedLeft && std::abs(Controls::Canvas::GetLeft(frame) - centeredLeft) <= 1) {
            if (!nativeLeft || nativeLeft == DependencyProperty::UnsetValue()) {
                frame.ClearValue(property);
            } else {
                frame.SetValue(property, nativeLeft);
            }
        }
        correctedLeft = false;
        nativeLeft = nullptr;
    }

    void RestoreRecommendationRows() {
        for (auto const& saved : recommendationRows) {
            if (auto row = saved.element.get()) {
                if (row.MinWidth() == saved.appliedWidth && row.MaxWidth() == saved.appliedWidth) {
                    row.MinWidth(saved.stockWidth);
                    row.MaxWidth(saved.stockWidth);
                }
            }
        }
        recommendationRows.clear();
    }

    void Restore() noexcept {
        bool restored = true;
        for (auto const& entry : pinnedHeights) {
            try {
                if (entry.token) {
                    entry.element.UnregisterPropertyChangedCallback(FrameworkElement::HeightProperty(), entry.token);
                }
            } catch (...) {
                restored = false;
                Log(L"Removing a pinned grid height callback failed: 0x%08X", winrt::to_hresult());
            }
        }
        pinnedHeights.clear();
        try {
            RestoreRecommendationRows();
        } catch (...) {
            restored = false;
            Log(L"Restoring recommended rows failed: 0x%08X", winrt::to_hresult());
        }
        try {
            RestoreAnchor();
        } catch (...) {
            restored = false;
            Log(L"Restoring the Start menu anchor failed: 0x%08X", winrt::to_hresult());
        }
        try {
            RestoreHorizontalCenter();
        } catch (...) {
            restored = false;
            Log(L"Restoring the Start menu horizontal position failed: 0x%08X", winrt::to_hresult());
        }
        for (auto it = values.rbegin(); it != values.rend(); ++it) {
            try {
                if (it->binding) {
                    Data::BindingOperations::SetBinding(it->element, it->property, it->binding);
                } else if (!it->localValue || it->localValue == DependencyProperty::UnsetValue()) {
                    it->element.ClearValue(it->property);
                } else {
                    it->element.SetValue(it->property, it->localValue);
                }
            } catch (...) {
                restored = false;
                Log(L"Restoring pinned grid property %zu failed: 0x%08X",
                    static_cast<size_t>(std::distance(it, values.rend()) - 1), winrt::to_hresult());
            }
        }
        try {
            WriteProperty(model.get(), setItemsPerRow, originalColumns);
            WriteProperty(model.get(), setItemsPerPage, originalPageSlots);
            unsigned columns;
            int pageSlots;
            ReadProperty(model.get(), getItemsPerRow, &columns);
            ReadProperty(model.get(), getItemsPerPage, &pageSlots);
            restored = restored && columns == originalColumns && pageSlots == originalPageSlots;
        } catch (...) {
            restored = false;
            Log(L"Restoring the pinned grid model failed: 0x%08X", winrt::to_hresult());
        }
        if (!restored) Log(L"Start menu restoration was incomplete.");
    }
};

std::optional<Layout> active;

void RestoreLayout() {
    if (active) {
        active->Restore();
        active.reset();
    }
}

HWND FindStartWindow() {
    HWND result = nullptr;
    EnumWindows([](HWND hwnd, LPARAM context) -> BOOL {
        DWORD processId = 0;
        GetWindowThreadProcessId(hwnd, &processId);
        wchar_t className[64];
        if (processId == GetCurrentProcessId() &&
            GetClassNameW(hwnd, className, ARRAYSIZE(className)) &&
            wcscmp(className, L"Windows.UI.Core.CoreWindow") == 0) {
            *reinterpret_cast<HWND*>(context) = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}

void OnStartThread(UiCallback callback) {
    HWND window = FindStartWindow();
    DWORD threadId = GetWindowThreadProcessId(window, nullptr);
    if (!threadId) {
        return;
    }
    if (threadId == GetCurrentThreadId()) {
        callback();
        return;
    }
    static const UINT message = RegisterWindowMessageW(L"StartMenuGrid.MainThread.v2");
    if (!message) {
        return;
    }
    std::scoped_lock dispatch(uiCallbackMutex);
    uiCallback.store(callback);
    HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC,
        [](int code, WPARAM wparam, LPARAM lparam) -> LRESULT {
            if (code == HC_ACTION) {
                auto event = reinterpret_cast<const CWPSTRUCT*>(lparam);
                if (event->message == message) {
                    if (auto run = uiCallback.exchange(nullptr)) {
                        run();
                    }
                }
            }
            return CallNextHookEx(nullptr, code, wparam, lparam);
        }, nullptr, threadId);
    if (hook) {
        SendMessageW(window, message, 0, 0);
        UnhookWindowsHookEx(hook);
    }
    uiCallback.store(nullptr);
}

FrameworkElement Find(FrameworkElement const& element, std::wstring_view name, std::wstring_view type = {}) {
    if ((name.empty() || element.Name() == name) && (type.empty() || winrt::get_class_name(element) == type)) {
        return element;
    }
    int count = Media::VisualTreeHelper::GetChildrenCount(element);
    for (int i = 0; i < count; ++i) {
        auto child = Media::VisualTreeHelper::GetChild(element, i).try_as<FrameworkElement>();
        if (child) {
            if (auto result = Find(child, name, type)) {
                return result;
            }
        }
    }
    return nullptr;
}

void UpdateRecommendationRows(Layout& layout) {
    if (layout.recommendationExtraWidth <= 0) {
        if (!layout.recommendationRows.empty()) {
            layout.RestoreRecommendationRows();
        }
        return;
    }
    if (!layout.recommendations) {
        layout.recommendations = Find(layout.frame, L"MoreSuggestionsRoot");
    }
    if (!layout.recommendations || layout.recommendations.ActualWidth() <= 0) {
        return;
    }
    auto list = Find(layout.recommendations, L"RecommendedList", L"Windows.UI.Xaml.Controls.ListView");
    auto panel = list ? list.as<Controls::ListView>().ItemsPanelRoot() : nullptr;
    if (!panel) {
        return;
    }

    auto& rows = layout.recommendationRows;
    std::erase_if(rows, [](auto const& saved) { return !saved.element.get(); });
    for (auto const& child : panel.Children()) {
        auto row = child.try_as<Controls::ListViewItem>();
        if (!row || !std::wstring_view(row.Name()).starts_with(L"RecommendedItemRoot")) {
            continue;
        }
        const double rowWidth = row.MinWidth();
        if (rowWidth <= 0 || !std::isfinite(rowWidth) || rowWidth != row.MaxWidth()) {
            continue;
        }
        auto saved = std::find_if(rows.begin(), rows.end(), [&](auto const& entry) {
            return entry.element.get() == row;
        });
        if (saved == rows.end()) {
            saved = rows.insert(rows.end(), RecommendationRow{winrt::make_weak(row), rowWidth, rowWidth});
        } else if (rowWidth != saved->appliedWidth) {
            // React Native can prepare a recycled item with its stock constraint.
            saved->stockWidth = rowWidth;
        }
        const double targetWidth = saved->stockWidth + layout.recommendationExtraWidth;
        if (rowWidth != targetWidth) {
            row.MinWidth(targetWidth);
            row.MaxWidth(targetWidth);
        }
        saved->appliedWidth = targetWidth;
    }
}

bool ApplyLayout() {
    int columns = currentSettings.columns;
    int rows = currentSettings.rows;
    if (columns == 0 && rows == 0) {
        RestoreLayout();
        ReportLayout(0, 1);
        return true;
    }
    if (columns < 1 || columns > 24 || rows < 1 || rows > 16) {
        RestoreLayout();
        ReportLayout(0, 2);
        return true;
    }

    if (content.ActualWidth() <= 0) {
        return false;
    }
    auto frame = Find(content, L"", L"StartDocked.StartSizingFrame");
    auto list = frame ? Find(frame, L"StartMenuPinnedList", L"StartMenu.PinnedList") : nullptr;
    auto view = list ? Find(list, L"PinnedList", L"Windows.UI.Xaml.Controls.GridView") : nullptr;
    auto wrapElement = view ? Find(view, L"", L"Windows.UI.Xaml.Controls.ItemsWrapGrid") : nullptr;
    if (!frame || !wrapElement || frame.ActualWidth() <= 0 || frame.ActualHeight() <= 0) {
        return false;
    }
    const BYTE* image = GetSupportedStartMenu();
    if (!image) {
        ReportLayout(0, 3);
        return true;
    }
    auto wrap = wrapElement.as<Controls::ItemsWrapGrid>();
    auto model = GetGridModel(list, image);
    if (!model) {
        ReportLayout(0, 3);
        return true;
    }
    if (wrap.Orientation() != Controls::Orientation::Horizontal) {
        ReportLayout(0, 3);
        return true;
    }
    if (active && (active->frame != frame || active->wrap != wrap || active->model.get() != model.get())) {
        RestoreLayout();
    }
    if (!active) {
        unsigned originalColumns;
        int originalPageSlots;
        ReadProperty(model.get(), getItemsPerRow, &originalColumns);
        ReadProperty(model.get(), getItemsPerPage, &originalPageSlots);
        if (!originalColumns || originalPageSlots <= 0) {
            return false;
        }
        active.emplace(Layout{frame, wrap, model, frame.ActualWidth(), frame.ActualHeight(),
                              originalColumns, originalPageSlots, {}});
    }

    const double listWidth = tileWidth * columns + 4;
    const double listHeight = tileHeight * rows;
    const double frameWidth = std::max(active->originalWidth, listWidth + horizontalPadding - 4);
    const double frameHeight = std::max(active->originalHeight, listHeight + verticalPadding);
    if (frameWidth + windowBorder > content.ActualWidth() || frameHeight + windowBorder > content.ActualHeight()) {
        RestoreLayout();
        ReportLayout(0, 4);
        return true;
    }
    auto& layout = *active;
    layout.pinnedHeight = listHeight;
    layout.recommendationExtraWidth = frameWidth - layout.originalWidth;
    layout.Set(frame, FrameworkElement::WidthProperty(), frameWidth);
    layout.Set(frame, FrameworkElement::MinWidthProperty(), frameWidth);
    layout.Set(frame, FrameworkElement::MaxWidthProperty(), frameWidth);
    layout.AnchorHorizontalCenter(frameWidth);
    layout.Set(frame, FrameworkElement::HeightProperty(), frameHeight);
    layout.Set(frame, FrameworkElement::MinHeightProperty(), frameHeight);
    layout.Set(frame, FrameworkElement::MaxHeightProperty(), frameHeight);
    layout.AnchorToTaskbar(frameHeight);
    layout.Set(list, FrameworkElement::WidthProperty(), listWidth + 60);
    layout.KeepPinnedHeight(list);
    layout.Set(view, FrameworkElement::WidthProperty(), listWidth);
    layout.KeepPinnedHeight(view);
    if (auto presenter = Find(view, L"", L"Windows.UI.Xaml.Controls.ItemsPresenter")) {
        layout.KeepPinnedHeight(presenter);
    }
    layout.Set(wrap, Controls::ItemsWrapGrid::ItemWidthProperty(), tileWidth);
    layout.Set(wrap, Controls::ItemsWrapGrid::ItemHeightProperty(), tileHeight);
    layout.Set(wrap, Controls::ItemsWrapGrid::MaximumRowsOrColumnsProperty(), winrt::box_value(columns));

    WriteProperty(model.get(), setItemsPerRow, static_cast<unsigned>(columns));
    WriteProperty(model.get(), setItemsPerPage, columns * rows);
    unsigned actualColumns;
    int actualPageSlots;
    ReadProperty(model.get(), getItemsPerRow, &actualColumns);
    ReadProperty(model.get(), getItemsPerPage, &actualPageSlots);
    if (actualColumns != static_cast<unsigned>(columns) || actualPageSlots != columns * rows) {
        throw winrt::hresult_error(E_FAIL, L"Start did not retain the pinned grid dimensions.");
    }
    ReportLayout(static_cast<int>(actualColumns), rows);
    frame.InvalidateMeasure();
    return true;
}

void TryApply() noexcept {
    if (!attached || !pending) {
        return;
    }
    try {
        pending = false;
        if (!ApplyLayout()) {
            pending = true;
        }
    } catch (...) {
        Log(L"Start grid update failed: 0x%08X", winrt::to_hresult());
        RestoreLayout();
        ReportLayout(0, 5);
    }
}

void Attach() noexcept {
    if (attached) {
        return;
    }
    try {
        startWindow = Window::Current();
        if (!startWindow) {
            return;
        }
        content = startWindow.Content().try_as<FrameworkElement>();
        if (!content) {
            return;
        }
        if (!visibilityToken) {
            visibilityToken = startWindow.VisibilityChanged([](auto const&, auto const& args) {
                if (args.Visible()) {
                    pending = true;
                }
            });
        }
        layoutToken = content.LayoutUpdated([](auto const&, auto const&) {
            TryApply();
            if (active) {
                try {
                    UpdateRecommendationRows(*active);
                } catch (...) {
                    Log(L"Updating recommended rows failed: 0x%08X", winrt::to_hresult());
                }
            }
        });
        attached = true;
        TryApply();
    } catch (...) {
        Log(L"Start UI attachment failed: 0x%08X", winrt::to_hresult());
    }
}

void Detach() noexcept {
    attached = false;
    try {
        if (layoutToken) {
            content.LayoutUpdated(layoutToken);
            layoutToken = {};
        }
        if (visibilityToken) {
            startWindow.VisibilityChanged(visibilityToken);
            visibilityToken = {};
        }
    } catch (...) {
        Log(L"Start UI detach failed: 0x%08X", winrt::to_hresult());
    }
    RestoreLayout();
    ReportLayout(0, 1);
    content = nullptr;
    startWindow = nullptr;
}
} // namespace

// SPDX-License-Identifier: GPL-3.0-only
// Support for the fixed StartMenuExperienceHost diagnostic extension.
#include <ocidl.h>
#include <winternl.h>
#include <sddl.h>
#include <intrin.h>
#include <delayimp.h>
#include <string>
#include <new>
#include <charconv>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {
constexpr wchar_t productTag[] = L"StartMenuGrid.Layout.Status.2";
constexpr wchar_t hostExecutable[] = L"\\SystemApps\\Microsoft.Windows.StartMenuExperienceHost_cw5n1h2txyewy\\StartMenuExperienceHost.exe";
constexpr GUID tapClass{0xc418beb0, 0x594e, 0x434c, {0x8c, 0x27, 0x9d, 0x0a, 0xce, 0x8a, 0x35, 0xb5}};

bool IsStartHost() {
    wchar_t windows[MAX_PATH];
    wchar_t executable[MAX_PATH];
    UINT length = GetWindowsDirectoryW(windows, ARRAYSIZE(windows));
    DWORD actual = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    if (!length || length >= ARRAYSIZE(windows) || !actual || actual >= ARRAYSIZE(executable))
        return false;
    return _wcsicmp((std::wstring(windows) + hostExecutable).c_str(), executable) == 0;
}

std::wstring SettingsPath() {
    wchar_t path[MAX_PATH];
    auto module = reinterpret_cast<HMODULE>(&__ImageBase);
    DWORD size = GetModuleFileNameW(module, path, ARRAYSIZE(path));
    if (!size || size >= ARRAYSIZE(path)) return {};
    std::wstring root(path, size);
    root.resize(root.find_last_of(L'\\'));
    root.resize(root.find_last_of(L'\\'));

    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    alignas(TOKEN_USER) BYTE info[SECURITY_MAX_SID_SIZE + sizeof(TOKEN_USER)]{};
    DWORD written = 0;
    BOOL valid = GetTokenInformation(token, TokenUser, info, sizeof(info), &written);
    CloseHandle(token);
    if (!valid) return {};
    PWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(info)->User.Sid, &sid)) return {};
    std::wstring result = root + L"\\Profiles\\" + sid + L"\\grid.txt";
    LocalFree(sid);
    return result;
}

GridConfig ReadSettings(const std::wstring& path) {
    if (path.empty()) return {};
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    char text[48];
    DWORD count;
    BOOL read = ReadFile(file, text, sizeof(text), &count, nullptr);
    CloseHandle(file);
    if (!read || count < 3 || count >= sizeof(text)) return {};
    GridConfig settings;
    const char* end = text + count;
    auto columns = std::from_chars(text, end, settings.columns);
    if (columns.ec != std::errc{} || columns.ptr == end || *columns.ptr != ' ') return {};
    auto rows = std::from_chars(columns.ptr + 1, end, settings.rows);
    if (rows.ec != std::errc{}) return {};
    return settings;
}

// StartDocked.dll calls SetWindowRgn when arranging the Start flyout. Replace
// that module's own named import only, leaving other windows and DLLs alone.
class StartRegionImport {
    void** slot = nullptr;
    void* original = nullptr;
    void* replacement = nullptr;

    static bool Write(void** address, void* value) {
        DWORD access;
        if (!VirtualProtect(address, sizeof(*address), PAGE_READWRITE, &access)) return false;
        InterlockedExchangePointer(address, value);
        DWORD unused;
        VirtualProtect(address, sizeof(*address), access, &unused);
        return true;
    }

public:
    bool Install(void* hook) {
        if (slot) return true;
        auto base = reinterpret_cast<BYTE*>(GetModuleHandleW(L"StartDocked.dll"));
        if (!base) return false;
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE) return false;
        auto imported = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
        if (!imported.VirtualAddress) return false;
        auto descriptors = reinterpret_cast<const ImgDelayDescr*>(base + imported.VirtualAddress);
        for (auto entry = descriptors; entry->rvaDLLName; ++entry) {
            if (!(entry->grAttrs & dlattrRva) || !entry->rvaINT || !entry->rvaIAT) continue;
            auto library = reinterpret_cast<const char*>(base + entry->rvaDLLName);
            if (_stricmp(library, "ext-ms-win-ntuser-draw-l1-1-0.dll")) continue;
            auto names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + entry->rvaINT);
            auto target = reinterpret_cast<void**>(base + entry->rvaIAT);
            for (; names->u1.AddressOfData; ++names, ++target) {
                if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
                auto name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                if (strcmp(reinterpret_cast<const char*>(name->Name), "SetWindowRgn")) continue;
                // An unresolved delay import points to this module's own thunk.
                // The replacement always calls the regular User32 entry point.
                auto previous = reinterpret_cast<uintptr_t>(*target);
                auto moduleStart = reinterpret_cast<uintptr_t>(base);
                if (*target != reinterpret_cast<void*>(&::SetWindowRgn) &&
                    (previous < moduleStart || previous >= moduleStart + nt->OptionalHeader.SizeOfImage)) {
                    return false;
                }
                original = *target;
                replacement = hook;
                if (!Write(target, hook)) return false;
                slot = target;
                return true;
            }
        }
        return false;
    }

    void Uninstall() {
        if (slot && *slot == replacement) Write(slot, original);
        slot = nullptr;
    }
};

GridConfig currentSettings;
std::wstring configPath;
DispatcherTimer settingsTimer{nullptr};
StartRegionImport regionImport;
std::atomic<bool> sessionAttached{};

void ReportLayout(int columns, int rows) {
    if (HWND window = FindStartWindow()) {
        int value = columns * 256 + rows;
        SetPropW(window, productTag, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(value)));
    }
}

void CheckForSettings() noexcept {
    try {
        auto settings = ReadSettings(configPath);
        if (settings != currentSettings) {
            currentSettings = settings;
            pending = true;
            TryApply();
        }
    } catch (...) {
        ReportLayout(0, 5);
    }
}

void StartLocalSession() noexcept {
    try {
        if (settingsTimer) return;
        currentSettings = ReadSettings(configPath);
        if (!regionImport.Install(reinterpret_cast<void*>(SetStartWindowRegion))) {
            ReportLayout(0, 6);
            return;
        }
        settingsTimer = DispatcherTimer();
        settingsTimer.Interval(std::chrono::milliseconds(450));
        settingsTimer.Tick([](auto const&, auto const&) {
            if (!attached) Attach();
            CheckForSettings();
        });
        pending = true;
        Attach();
        settingsTimer.Start();
    } catch (...) {
        ReportLayout(0, 5);
    }
}

void StopLocalSession() noexcept {
    try {
        if (settingsTimer) {
            settingsTimer.Stop();
            settingsTimer = nullptr;
        }
    } catch (...) {
        Log(L"Stopping the Start menu timer failed");
    }
    Detach();
    regionImport.Uninstall();
    if (HWND window = FindStartWindow()) RemovePropW(window, productTag);
}

class Tap final : public IObjectWithSite {
    std::atomic<ULONG> references{1};
    winrt::com_ptr<IUnknown> site;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (iid != IID_IUnknown && iid != IID_IObjectWithSite) return E_NOINTERFACE;
        *value = static_cast<IObjectWithSite*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = --references;
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* value) override try {
        site.copy_from(value);
        if (!value) {
            sessionAttached.store(false);
            OnStartThread(StopLocalSession);
            return S_OK;
        }
        if (!IsStartHost()) return E_ACCESSDENIED;
        configPath = SettingsPath();
        if (configPath.empty()) return E_FAIL;
        HMODULE pinned;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&__ImageBase), &pinned)) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        sessionAttached.store(true);
        HANDLE helper = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            for (int attempt = 0; attempt < 20 && sessionAttached.load(); ++attempt) {
                if (FindStartWindow()) {
                    OnStartThread([] {
                        if (sessionAttached.load()) StartLocalSession();
                    });
                    return 0;
                }
                Sleep(150);
            }
            return 1;
        }, nullptr, 0, nullptr);
        if (!helper) return HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(helper);
        return S_OK;
    } catch (...) {
        return winrt::to_hresult();
    }
    HRESULT STDMETHODCALLTYPE GetSite(REFIID iid, void** value) override {
        return site ? site->QueryInterface(iid, value) : E_FAIL;
    }
};

class TapFactory final : public IClassFactory {
    std::atomic<ULONG> references{1};
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (iid != IID_IUnknown && iid != IID_IClassFactory) return E_NOINTERFACE;
        *value = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = --references;
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** value) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto tap = new (std::nothrow) Tap;
        if (!tap) return E_OUTOFMEMORY;
        HRESULT result = tap->QueryInterface(iid, value);
        tap->Release();
        return result;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
};
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** value) {
    if (clsid != tapClass || !IsStartHost()) return CLASS_E_CLASSNOTAVAILABLE;
    auto factory = new (std::nothrow) TapFactory;
    if (!factory) return E_OUTOFMEMORY;
    HRESULT result = factory->QueryInterface(iid, value);
    factory->Release();
    return result;
}

STDAPI DllCanUnloadNow() {
    return sessionAttached.load() ? S_FALSE : S_OK;
}
