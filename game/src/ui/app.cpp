#include "app.hpp"

namespace ui {

App& app() {
    static App instance;
    return instance;
}

}  // namespace ui
