#pragma once
#include "resource_manager.h"
#include "search_bar_controller.h"

class App;

// ResourceManager と SearchBarController が App に問い合わせる窓口。定義は app_callbacks.cpp。
class AppHost final : public ResourceManagerHost, public SearchBarHost {
public:
    explicit AppHost(App& app) noexcept : app_(app)
    {}

    void SetTimer(app_timer::Id id, UINT ms) override;
    void KillTimer(app_timer::Id id) override;
    float MdContentWidth() override;
    float MdPaneHeight() override;
    float MdMaxScroll() override;
    const Theme& GetTheme() override;
    void RecomputeLayout(layout::HeightChangeRange changed) override;
    void EnsureNodeLayout(int node_index) override;

private:
    App& app_;
};
