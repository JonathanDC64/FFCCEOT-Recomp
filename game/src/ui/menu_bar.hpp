// Adapted from Dusklight (CC0), src/dusk/ui/menu_bar.hpp
#pragma once

#include "button.hpp"
#include "document.hpp"
#include "tab_bar.hpp"

#include <memory>

namespace ui {

class MenuBar : public Document {
public:
    MenuBar();

    MenuBar(const MenuBar&) = delete;
    MenuBar& operator=(const MenuBar&) = delete;

    void show() override;
    void hide(bool close) override;
    void update() override;
    bool focus() override;
    bool visible() const override;
    bool permanent() const override { return true; }

    static void refresh_tabs();

protected:
    bool handle_nav_command(Rml::Event& event, NavCommand cmd) override;

private:
    void build_tabs();
    void update_safe_area() noexcept;

    Rml::Element* mRoot;
    std::unique_ptr<TabBar> mTabBar;
    std::unique_ptr<Button> mCloseButton;
    Insets mTabBarPadding;
    Rml::String mFocusedTabTitle;
};

}  // namespace ui
