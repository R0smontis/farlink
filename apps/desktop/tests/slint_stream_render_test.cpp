#include "crossdesk_ui.h"
#include "ui/model_update.h"

#include <slint-platform.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
// Match the retained software buffer used by the Harmony desktop adapter.
// A full window snapshot would conceal missing damage/redraw notifications.
class TestWindow : public slint::platform::WindowAdapter {
 public:
  slint::platform::SoftwareRenderer software{
      slint::platform::SoftwareRenderer::RepaintBufferType::ReusedBuffer};
  std::vector<slint::Rgb8Pixel> pixels = std::vector<slint::Rgb8Pixel>(1280 * 720);
  bool dirty = true;
  slint::PhysicalSize size() override { return slint::PhysicalSize({1280, 720}); }
  slint::platform::AbstractRenderer& renderer() override { return software; }
  void request_redraw() override { dirty = true; }
  void Render() {
    if (!dirty && !window().has_active_animations()) return;
    dirty = false;
    software.render(pixels, 1280);
  }
};

class TestPlatform : public slint::platform::Platform {
 public:
  TestWindow* adapter = nullptr;
  std::unique_ptr<slint::platform::WindowAdapter> create_window_adapter() override {
    auto result = std::make_unique<TestWindow>();
    adapter = result.get();
    return result;
  }
};
}  // namespace

int main() {
  auto platform = std::make_unique<TestPlatform>();
  auto* backend = platform.get();
  slint::platform::set_platform(std::move(platform));
  auto stream = crossdesk::ui::StreamWindow::create();
  auto& adapter = *backend->adapter;
  stream->window().dispatch_resize_event(slint::LogicalSize({1280, 720}));
  stream->show();
  stream->set_secure_attention_supported(false);
  stream->set_privacy_visible(true);
  auto rows = std::make_shared<slint::VectorModel<crossdesk::ui::NetworkStatsRow>>();
  stream->set_stats_rows(rows);
  std::vector<crossdesk::ui::NetworkStatsRow> stats(4);
  unsigned samples = 0;
  bool stats_visible = false;
  stream->on_toggle_stats([&] { stats_visible = !stats_visible; });
  // Synchronize inside a Slint timer, exactly as GuiApplication does. Updating
  // before update_timers_and_animations would use the previous animation tick.
  slint::Timer sync;
  sync.start(slint::TimerMode::Repeated, std::chrono::milliseconds(16), [&] {
    stream->set_stats_visible(stats_visible);
    // Keep values changing while the panel animates, as during a live session.
    stats[0].inbound = slint::SharedString(std::to_string(++samples) + " kbps");
    crossdesk::ui::UpdateModelRows(*rows, stats);
  });
  const auto tick = [&] {
    slint::platform::update_timers_and_animations();
    adapter.Render();
  };
  const auto settle = [&](int frames = 12) {
    for (int i = 0; i < frames; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
      tick();
    }
  };
  settle();
  const float top = stream->get_video_area_top();
  // The initial control is only the logo. Wait for the toolbar reveal before
  // clicking network status, just as a user does in a new session.
  const slint::LogicalPosition logo({19, top + 19});
  stream->window().dispatch_pointer_move_event(logo);
  stream->window().dispatch_pointer_press_event(logo, slint::PointerEventButton::Left);
  stream->window().dispatch_pointer_release_event(logo, slint::PointerEventButton::Left);
  settle(20);
  for (bool dock_right : {false, true}) {
    if (dock_right) {
      stream->invoke_begin_control_drag(5, 20);
      stream->invoke_move_control_drag(1275, 20);
      stream->invoke_end_control_drag();
      settle();
    }
    // With privacy visible and no Windows shortcuts, the toolbar is 344 px.
    // Its logo moves to the trailing edge when docked on the right.
    const int control_x = dock_right ? int(stream->get_video_area_width()) - 344 : 0;
    const slint::LogicalPosition button({float(control_x + (dock_right ? 214 : 250)), top + 19});
    stream->window().dispatch_pointer_move_event(button);
    settle(35); // Include the tooltip that appears before the click.
    for (bool expanded : {true, false, true, false}) {
      stream->window().dispatch_pointer_press_event(button, slint::PointerEventButton::Left);
      stream->window().dispatch_pointer_release_event(button, slint::PointerEventButton::Left);
      settle(); // No pointer movement: animation must finish on its own.
      if (stats_visible != expanded) {
        std::cerr << "Network status button did not toggle the panel; dock_right="
                  << dock_right << '\n';
        return 1;
      }
      for (int y : {65, 195}) {
        const auto pixel = adapter.pixels[(int(top) + y) * 1280 + control_x + 4];
        const bool painted = pixel.r >= 240 && pixel.g >= 240 && pixel.b >= 240;
        const bool cleared = pixel.r == 0 && pixel.g == 0 && pixel.b == 0;
        if (expanded ? !painted : !cleared) {
          std::cerr << "Stats must fully " << (expanded ? "expand" : "collapse")
                    << " without pointer movement; dock_right=" << dock_right
                    << ", y=" << y << ", pixel=" << int(pixel.r) << ','
                    << int(pixel.g) << ',' << int(pixel.b) << '\n';
          return 1;
        }
        if (stream->invoke_is_local_control_area(control_x + 4, y) != expanded) {
          std::cerr << "Stats input area must match the rendered panel\n";
          return 1;
        }
      }
    }
  }

  // --- Game-mouse control: capability gating, state feedback, callback wiring ---
  // The game-mouse button is the user-facing surface for the relative-mouse /
  // game mode. Verify, at the binding level the runtime drives each sync tick:
  //   * capability gate: game_mouse_available is what reveals the button;
  //   * state: game_mouse_active is what the runtime sets while captured;
  //   * the toggle callback fires so the runtime can enter/exit the mode;
  //   * the localized state string (release_game_mouse) is bound so the
  //     transient entry notice can convey the escape action.
  stream->set_game_mouse_available(false);
  stream->set_game_mouse_active(false);
  settle();
  if (stream->get_game_mouse_available() || stream->get_game_mouse_active()) {
    std::cerr << "Game-mouse properties must round-trip to false\n";
    return 1;
  }
  // Capability present -> the control becomes available.
  stream->set_game_mouse_available(true);
  settle();
  if (!stream->get_game_mouse_available()) {
    std::cerr << "game_mouse_available must round-trip to true\n";
    return 1;
  }
  // The escape/state string must round-trip so the notice can show it. The
  // localized strings live on the StreamStrings global, exactly where
  // ui_localization writes them.
  auto &stream_strings = stream->global<crossdesk::ui::StreamStrings>();
  stream_strings.set_release_game_mouse(slint::SharedString("Release Game Mouse (Ctrl+Alt+G)"));
  if (std::string(stream_strings.get_release_game_mouse()) != "Release Game Mouse (Ctrl+Alt+G)") {
    std::cerr << "release-game-mouse string must round-trip\n";
    return 1;
  }
  // The toggle callback is how the runtime enters/exits the mode.
  bool game_toggled = false;
  stream->on_toggle_game_mouse([&] { game_toggled = !game_toggled; });
  stream->invoke_toggle_game_mouse();
  if (!game_toggled) {
    std::cerr << "toggle-game-mouse callback must fire\n";
    return 1;
  }
  // --- Transient entry notice ---
  // While captured the hardware cursor is frozen, so neither the toolbar
  // button nor its tooltip is reachable. The escape chord is instead shown
  // once as a transient surface when capture begins. It must:
  //   * be hidden before entry;
  if (stream->get_game_mouse_notice_visible()) {
    std::cerr << "Game-mouse notice must be hidden before capture\n";
    return 1;
  }
  //   * appear the moment capture is entered;
  stream->set_game_mouse_active(true);
  settle();
  if (!stream->get_game_mouse_active() || !stream->get_game_mouse_notice_visible()) {
    std::cerr << "Game-mouse notice must appear on capture entry\n";
    return 1;
  }
  //   * stay up through a short capture (the notice must not vanish
  //     instantly);
  settle(10);  // ~160ms into the 4s window
  if (!stream->get_game_mouse_notice_visible()) {
    std::cerr << "Game-mouse notice must not expire before the timeout\n";
    return 1;
  }
  //   * expire on its own after the 4s timeout. Timers follow wall time;
  //     keep pumping until the deadline passes, exactly as the event loop
  //     would during a live capture.
  settle(280);  // entry + ~4.8s total
  if (stream->get_game_mouse_notice_visible()) {
    std::cerr << "Game-mouse notice must expire after the timeout\n";
    return 1;
  }
  //   * reappear on a later entry and clear immediately on exit.
  stream->set_game_mouse_active(false);
  settle();
  stream->set_game_mouse_active(true);
  settle();
  if (!stream->get_game_mouse_notice_visible()) {
    std::cerr << "Game-mouse notice must reappear on re-entry\n";
    return 1;
  }
  stream->set_game_mouse_active(false);
  settle();
  if (stream->get_game_mouse_notice_visible()) {
    std::cerr << "Game-mouse notice must clear the moment capture ends\n";
    return 1;
  }
  std::cout << "Game-mouse notice binding passed.\n";
  std::cout << "Game-mouse control binding passed.\n";
  std::cout << "Stream stats retained-buffer rendering passed.\n";
  return 0;
}
