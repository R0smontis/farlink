#include "keyboard_capturer.h"

#include <cstdint>
#include <unordered_map>

#include "keyboard_converter.h"
#include "../mac_injected_event.h"
#include "rd_log.h"

namespace crossdesk {

static OnKeyAction g_on_key_action = nullptr;
static void* g_user_ptr = nullptr;
static std::unordered_map<int, int> g_unmapped_keycode_to_vk;

static int VkCodeFromUnicode(UniChar ch) {
  if (ch >= 'a' && ch <= 'z') {
    return static_cast<int>(ch - 'a' + 'A');
  }
  if (ch >= 'A' && ch <= 'Z') {
    return static_cast<int>(ch);
  }
  if (ch >= '0' && ch <= '9') {
    return static_cast<int>(ch);
  }

  switch (ch) {
    case ' ':
      return 0x20;  // VK_SPACE
    case '-':
    case '_':
      return 0xBD;  // VK_OEM_MINUS
    case '=':
    case '+':
      return 0xBB;  // VK_OEM_PLUS
    case '[':
    case '{':
      return 0xDB;  // VK_OEM_4
    case ']':
    case '}':
      return 0xDD;  // VK_OEM_6
    case '\\':
    case '|':
      return 0xDC;  // VK_OEM_5
    case ';':
    case ':':
      return 0xBA;  // VK_OEM_1
    case '\'':
    case '"':
      return 0xDE;  // VK_OEM_7
    case ',':
    case '<':
      return 0xBC;  // VK_OEM_COMMA
    case '.':
    case '>':
      return 0xBE;  // VK_OEM_PERIOD
    case '/':
    case '?':
      return 0xBF;  // VK_OEM_2
    case '`':
    case '~':
      return 0xC0;  // VK_OEM_3
    default:
      return -1;
  }
}

static int ResolveVkCodeFromMacEvent(CGEventRef event, CGKeyCode key_code,
                                     bool is_key_down) {
  auto key_it = CGKeyCodeToVkCode.find(key_code);
  if (key_it != CGKeyCodeToVkCode.end()) {
    if (is_key_down) {
      g_unmapped_keycode_to_vk.erase(static_cast<int>(key_code));
    }
    return key_it->second;
  }

  int vk_code = -1;
  UniChar chars[4] = {0};
  UniCharCount char_count = 0;
  CGEventKeyboardGetUnicodeString(event, 4, &char_count, chars);
  if (char_count > 0) {
    vk_code = VkCodeFromUnicode(chars[0]);
  }

  if (vk_code < 0) {
    auto fallback_it =
        g_unmapped_keycode_to_vk.find(static_cast<int>(key_code));
    if (fallback_it != g_unmapped_keycode_to_vk.end()) {
      vk_code = fallback_it->second;
    }
  }

  if (vk_code >= 0) {
    if (is_key_down) {
      g_unmapped_keycode_to_vk[static_cast<int>(key_code)] = vk_code;
    } else {
      g_unmapped_keycode_to_vk.erase(static_cast<int>(key_code));
    }
  }

  return vk_code;
}

CGEventRef eventCallback(CGEventTapProxy proxy, CGEventType type,
                         CGEventRef event, void* userInfo) {
  (void)proxy;
  PlatformKeyboardCapturer* keyboard_capturer = (PlatformKeyboardCapturer*)userInfo;

  // A tap the system disabled stops observing everything — including the
  // keys that would re-enable it — so the re-enable must happen here, on
  // the tap thread, exactly like the mouse capturer's. Returning the event
  // (not nullptr) because a disabled tap cannot swallow anyway.
  if (type == kCGEventTapDisabledByTimeout ||
      type == kCGEventTapDisabledByUserInput) {
    if (keyboard_capturer) keyboard_capturer->ReenableTap();
    return event;
  }
  if (CGEventGetIntegerValueField(event, kCGEventSourceUserData) ==
      kCrossDeskInjectedEvent) {
    return event;
  }
  if (!g_on_key_action) {
    return event;
  }

  if (!keyboard_capturer) {
    LOG_ERROR("keyboard_capturer is nullptr");
    return event;
  }

  if (type == kCGEventKeyDown || type == kCGEventKeyUp) {
    const bool is_key_down = (type == kCGEventKeyDown);
    CGKeyCode key_code = static_cast<CGKeyCode>(
        CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
    int vk_code = ResolveVkCodeFromMacEvent(event, key_code, is_key_down);
    if (vk_code < 0) {
      return nullptr;
    }
    if (keyboard_capturer->game_mode_active_.load(std::memory_order_acquire)) {
      // Swallow-first ordering (see GameModeExitChord): the G key-up that
      // ends the swallow state is itself swallowed.
      if (keyboard_capturer->exit_chord_.SwallowsKey(vk_code)) {
        keyboard_capturer->exit_chord_.Update(vk_code, is_key_down);
        return nullptr;
      }
      if (keyboard_capturer->exit_chord_.Update(vk_code, is_key_down)) {
        return nullptr;
      }
    }
    g_on_key_action(vk_code, is_key_down, 0, false, g_user_ptr);
  } else if (type == kCGEventFlagsChanged) {
    const CGEventFlags current_flags = CGEventGetFlags(event);
    CGKeyCode key_code = static_cast<CGKeyCode>(
        CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
    MacModifierKeyTracker::Emission emission;
    if (!keyboard_capturer->modifier_tracker_.Update(
            static_cast<int>(key_code), static_cast<uint64_t>(current_flags),
            emission)) {
      return nullptr;
    }
    if (keyboard_capturer->game_mode_active_.load(std::memory_order_acquire) &&
        (emission.vk == 0xA2 || emission.vk == 0xA3 || emission.vk == 0xA4 ||
         emission.vk == 0xA5)) {
      // Ctrl/Alt feed the chord tracker but are never swallowed themselves:
      // in game mode they are ordinary input the peer expects, and the
      // chord is defined by G alone.
      keyboard_capturer->exit_chord_.Update(emission.vk, emission.is_down);
    }
    g_on_key_action(emission.vk, emission.is_down, 0, false, g_user_ptr);
  }

  return nullptr;
}

PlatformKeyboardCapturer::PlatformKeyboardCapturer()
    : event_tap_(nullptr), run_loop_source_(nullptr) {}

PlatformKeyboardCapturer::~PlatformKeyboardCapturer() { Unhook(); }

int PlatformKeyboardCapturer::Hook(OnKeyAction on_key_action, void* user_ptr) {
  if (event_tap_) {
    return 0;
  }

  g_unmapped_keycode_to_vk.clear();
  g_on_key_action = on_key_action;
  g_user_ptr = user_ptr;
  modifier_tracker_.Clear();
  exit_chord_.Reset();

  CGEventMask eventMask = (1 << kCGEventKeyDown) | (1 << kCGEventKeyUp) |
                          (1 << kCGEventFlagsChanged);

  // PreferLocal creates a listen-only tap: events are observed and forwarded
  // to the peer but never consumed, so local shortcuts keep working. The
  // option is fixed at creation time; switching modes requires re-hooking.
  const CGEventTapOptions options =
      shortcut_mode_ == ShortcutMode::kPreferLocal ? kCGEventTapOptionListenOnly
                                                   : kCGEventTapOptionDefault;
  event_tap_ = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
                                options, eventMask, eventCallback, this);

  if (!event_tap_) {
    LOG_ERROR("CGEventTapCreate failed");
    return -1;
  }

  run_loop_source_ =
      CFMachPortCreateRunLoopSource(kCFAllocatorDefault, event_tap_, 0);
  if (!run_loop_source_) {
    LOG_ERROR("CFMachPortCreateRunLoopSource failed");
    CFRelease(event_tap_);
    event_tap_ = nullptr;
    return -1;
  }

  CFRunLoopAddSource(CFRunLoopGetCurrent(), run_loop_source_,
                     kCFRunLoopCommonModes);

  // Only caps lock can be seeded: its bit is a latch that mirrors the
  // keyboard state exactly. The hold-modifier bits say nothing about which
  // side is down, and seeding both sides would fabricate presses the peer
  // never sees releases for; a modifier already held at hook time simply
  // produces no press edge, and its release is suppressed as a non-edge.
  const CGEventFlags current_flags =
      CGEventSourceFlagsState(kCGEventSourceStateCombinedSessionState);
  modifier_tracker_.SeedCapsLock(
      (current_flags & kCGEventFlagMaskAlphaShift) != 0);

  CGEventTapEnable(event_tap_, true);
  return 0;
}

void PlatformKeyboardCapturer::ReenableTap() {
  if (event_tap_) CGEventTapEnable(event_tap_, true);
}

int PlatformKeyboardCapturer::Unhook() {
  g_unmapped_keycode_to_vk.clear();
  g_on_key_action = nullptr;
  g_user_ptr = nullptr;
  // Drop all captured-side state: a re-hook starts from a clean tracker,
  // and the controller force-releases whatever the peer still sees held.
  modifier_tracker_.Clear();
  exit_chord_.Reset();
  // The injected-modifier mirror was written in step with injected events;
  // with capture gone those events no longer arrive, so a stale mirror
  // would corrupt the flags of everything injected after re-hooking.
  injected_modifier_state_.Clear();

  if (event_tap_) {
    CGEventTapEnable(event_tap_, false);
  }

  if (run_loop_source_) {
    CFRunLoopRemoveSource(CFRunLoopGetCurrent(), run_loop_source_,
                          kCFRunLoopCommonModes);
    CFRelease(run_loop_source_);
    run_loop_source_ = nullptr;
  }

  if (event_tap_) {
    CFRelease(event_tap_);
    event_tap_ = nullptr;
  }

  return 0;
}

inline bool IsFunctionKey(int key_code) {
  switch (key_code) {
    case 0x7A:
    case 0x78:
    case 0x63:
    case 0x76:
    case 0x60:
    case 0x61:
    case 0x62:
    case 0x64:
    case 0x65:
    case 0x6D:
    case 0x67:
    case 0x6F:
      return true;
    default:
      return false;
  }
}

CGEventFlags ToCGEventFlags(uint32_t injected_flags) {
  CGEventFlags flags = 0;
  if ((injected_flags & kMacInjectedModifierShift) != 0) {
    flags |= kCGEventFlagMaskShift;
  }
  if ((injected_flags & kMacInjectedModifierControl) != 0) {
    flags |= kCGEventFlagMaskControl;
  }
  if ((injected_flags & kMacInjectedModifierOption) != 0) {
    flags |= kCGEventFlagMaskAlternate;
  }
  if ((injected_flags & kMacInjectedModifierCommand) != 0) {
    flags |= kCGEventFlagMaskCommand;
  }
  return flags;
}

int PlatformKeyboardCapturer::SendKeyboardCommand(int key_code, bool is_down,
                                          uint32_t scan_code, bool extended) {
  (void)scan_code;
  (void)extended;
  const uint32_t injected_flags =
      injected_modifier_state_.Update(key_code, is_down);

  if (vkCodeToCGKeyCode.find(key_code) != vkCodeToCGKeyCode.end()) {
    CGKeyCode cg_key_code = vkCodeToCGKeyCode[key_code];
    CGEventRef event = CGEventCreateKeyboardEvent(NULL, cg_key_code, is_down);
    if (!event) {
      LOG_ERROR("CGEventCreateKeyboardEvent failed");
      return -1;
    }

    // Caps Lock is a session toggle. Replacing every flag with the remote
    // held modifiers also cleared Caps Lock on ordinary letter events.
    const CGEventFlags caps_lock =
        CGEventSourceFlagsState(kCGEventSourceStateCombinedSessionState) &
        kCGEventFlagMaskAlphaShift;
    CGEventSetFlags(event, ToCGEventFlags(injected_flags) | caps_lock);
    CGEventSetIntegerValueField(event, kCGEventSourceUserData,
                               kCrossDeskInjectedEvent);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);

    // F1-F12 keys often require the FN key to be pressed on Mac keyboards, so
    // we simulate the FN key release when an F1-F12 key is released.
    if (IsFunctionKey(cg_key_code) && !is_down) {
      CGEventRef fn_release_event =
          CGEventCreateKeyboardEvent(NULL, fn_key_code_, false);
      if (!fn_release_event) {
        LOG_ERROR("CGEventCreateKeyboardEvent failed for fn release");
        return -1;
      }
      CGEventSetIntegerValueField(fn_release_event, kCGEventSourceUserData,
                                 kCrossDeskInjectedEvent);
      CGEventPost(kCGHIDEventTap, fn_release_event);
      CFRelease(fn_release_event);
    }
  }

  return 0;
}
int PlatformKeyboardCapturer::SendTextCommand(const std::string& text) {
  if (text.empty() || text.size() > kMaxTextInputBytes) return -1;
  CFStringRef string = CFStringCreateWithBytes(
      kCFAllocatorDefault, reinterpret_cast<const UInt8*>(text.data()),
      text.size(), kCFStringEncodingUTF8, false);
  if (!string) return -1;
  const CFIndex length = CFStringGetLength(string);
  std::vector<UniChar> characters(length);
  CFStringGetCharacters(string, CFRangeMake(0, length), characters.data());
  CFRelease(string);
  for (CFIndex index = 0; index < length;) {
    const UniChar character = characters[index];
    if (character == '\n' || character == '\r' || character == '\t') {
      const int key = character == '\t' ? 0x09 : 0x0D;
      SendKeyboardCommand(key, true, 0, false);
      SendKeyboardCommand(key, false, 0, false);
      ++index;
      if (character == '\r' && index < length && characters[index] == '\n') ++index;
      continue;
    }
    // Keep surrogate pairs together. Separate events let apps handle long
    // dictation results without CGEvent's small inline Unicode buffer limit.
    const CFIndex count = CFStringIsSurrogateHighCharacter(character) &&
                                  index + 1 < length ? 2 : 1;
    CGEventRef down = CGEventCreateKeyboardEvent(nullptr, 0, true);
    CGEventRef up = CGEventCreateKeyboardEvent(nullptr, 0, false);
    if (!down || !up) {
      if (down) CFRelease(down);
      if (up) CFRelease(up);
      return -1;
    }
    for (CGEventRef event : {down, up}) {
      CGEventSetFlags(event, 0);
      CGEventKeyboardSetUnicodeString(event, count, characters.data() + index);
      CGEventSetIntegerValueField(event, kCGEventSourceUserData,
                                 kCrossDeskInjectedEvent);
      CGEventPost(kCGHIDEventTap, event);
      CFRelease(event);
    }
    index += count;
  }
  return 0;
}

}  // namespace crossdesk
