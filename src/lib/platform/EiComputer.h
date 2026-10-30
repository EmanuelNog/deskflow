/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2024, 2026 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2022, 2026 Red Hat, Inc.
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/IComputer.h"
#include "deskflow/PlatformComputer.h"
#ifdef HAVE_LIBPORTAL_SHORTCUTS
#include "platform/PortalGlobalShortcuts.h"
#endif
#include "platform/XDGPowerManager.h"

#include <bitset>
#include <climits>
#include <libei.h>
#include <map>
#include <mutex>
#include <vector>

struct ei;
struct ei_event;
struct ei_seat;
struct ei_device;

class EventQueueTimer;

namespace deskflow {

class EiKeyState;
class PortalRemoteDesktop;
class PortalInputCapture;
class PortalGlobalShortcuts;
class EiClipboard;

using ClipboardInfo = IComputer::ClipboardInfo;

//! Implementation of IPlatformComputer for X11
class EiComputer : public PlatformComputer
{
  friend class EiHotKeyModifierMatchingTests;

public:
  EiComputer(bool isPrimary, IEventQueue *events, bool usePortal);
  ~EiComputer() override;

  // IComputer overrides
  void *getEventTarget() const final;
  bool getClipboard(ClipboardID id, IClipboard *) const override;
  void getShape(std::int32_t &x, std::int32_t &y, std::int32_t &width, std::int32_t &height) const override;
  void getCursorPos(std::int32_t &x, std::int32_t &y) const override;

  // IPrimaryComputer overrides
  void reconfigure(std::uint32_t activeSides) override;
  std::uint32_t activeSides() override;
  void warpCursor(std::int32_t x, std::int32_t y) override;
  std::uint32_t registerHotKey(KeyID key, KeyModifierMask mask) override;
  void unregisterHotKey(std::uint32_t id) override;
  void fakeInputBegin() override;
  void fakeInputEnd() override;
  std::int32_t getJumpZoneSize() const override;
  bool isAnyMouseButtonDown(std::uint32_t &buttonID) const override;
  void getCursorCenter(std::int32_t &x, std::int32_t &y) const override;

  // ISecondaryComputer overrides
  void fakeMouseButton(ButtonID id, bool press) override;
  void fakeMouseMove(std::int32_t x, std::int32_t y) override;
  void fakeMouseRelativeMove(std::int32_t dx, std::int32_t dy) const override;
  void fakeMouseWheel(ScrollDelta delta) const override;
  void fakeKey(std::uint32_t keycode, bool isDown) const;

  // IPlatformComputer overrides
  void enable() override;
  void disable() override;
  void enter() override;
  bool canLeave() override;
  void leave() override;
  bool setClipboard(ClipboardID, const IClipboard *) override;
  void checkClipboards() override;
  void openScreensaver(bool notify) override;
  void closeScreensaver() override;
  void screensaver(bool activate) override;
  void resetOptions() override;
  void setOptions(const OptionsList &options) override;
  void setSequenceNumber(std::uint32_t) override;
  bool isPrimary() const override;

  // Send clipboard event (needed by PortalInputCapture)
  void sendClipboardEvent(EventTypes type, ClipboardID id) const;

  /*!
   * Whether a device that shows up while a session is already running may be handed to
   * ei_device_start_emulating() as soon as it arrives.
   *
   * libei documents that call as "only available on an ei sender context", so the primary -
   * which is a receiver - must never make it: libeis answers a start from a receiver with
   * "Invalid event from receiver ei context. Disconnecting client", and the device then flaps
   * PAUSED <-> RESUMED with none of its events delivered.  On the sender (the secondary) a late
   * device does need the call, but only once EIS has resumed it - starting a paused device is
   * the same invalid event.
   */
  static bool canStartEmulatingOnArrival(bool isPrimary, bool resumed);

  //! Which of the three devices Deskflow drives a given ei_device is.  `Untracked` means a device
  //! the capture session holds but Deskflow does not track (a touchscreen, a second tablet).
  enum class EmulationSlot
  {
    Untracked,
    Pointer,
    Keyboard,
    Absolute
  };

  /*!
   * Per-device emulation bookkeeping.
   *
   * ei_device_start_emulating() is per device, and libeis keeps the state per device: a second
   * start on a device it already holds in EMULATING is answered with "Invalid device state N
   * for a start_emulating event" and a disconnect, which rebuilds the whole capture (an input
   * stall).  A single bool cannot express that - any pause cleared it for every device, so the
   * next injected input restarted devices that had never stopped.
   */
  struct EmulationState
  {
    bool pointer = false;
    bool keyboard = false;
    bool absolute = false;

    //! The devices a start pass must start: resumed, and not already emulating.
    struct StartSet
    {
      bool pointer = false;
      bool keyboard = false;
      bool absolute = false;
    };

    bool any() const
    {
      return pointer || keyboard || absolute;
    }

    bool isEmulating(EmulationSlot slot) const
    {
      switch (slot) {
        case EmulationSlot::Pointer:
          return pointer;
        case EmulationSlot::Keyboard:
          return keyboard;
        case EmulationSlot::Absolute:
          return absolute;
        case EmulationSlot::Untracked:
          break;
      }
      return false;
    }

    void setEmulating(EmulationSlot slot, bool value)
    {
      switch (slot) {
        case EmulationSlot::Pointer:
          pointer = value;
          break;
        case EmulationSlot::Keyboard:
          keyboard = value;
          break;
        case EmulationSlot::Absolute:
          absolute = value;
          break;
        case EmulationSlot::Untracked:
          // Not ours: nothing to record.
          break;
      }
    }

    //! A pause clears ONLY the paused device. EIS resets that device to neutral by itself and
    //! sends no further events for it; every other device is untouched and stays emulating.
    void pause(EmulationSlot slot)
    {
      setEmulating(slot, false);
    }

    StartSet toStart(bool pointerResumed, bool keyboardResumed, bool absoluteResumed) const
    {
      return StartSet{
          pointerResumed && !pointer, keyboardResumed && !keyboard, absoluteResumed && !absolute
      };
    }
  };

  // Local clipboard cache (used by PortalRemoteDesktop to land selection reads)
  EiClipboard *getClipboardCache() const
  {
    return m_clipboard;
  }

  // Maximum clipboard size in KB, configured by the server
  size_t maximumClipboardSize() const
  {
    return m_maximumClipboardSize;
  }

protected:
  // IPlatformComputer overrides
  void handleSystemEvent(const Event &event) override;
  void updateButtons() override;
  IKeyState *getKeyState() const override;
  std::string getSecureInputApp() const override;

  void updateShape();
  void addDevice(ei_device *device);
  void removeDevice(ei_device *device);

private:
  void initEi();
  void cleanupEi();
  void sendEvent(EventTypes type, void *data);
  ButtonID mapButtonFromEvdev(ei_event *event) const;
  void onKeyEvent(ei_event *event);
  void onButtonEvent(ei_event *event);
  void sendWheelEvents(ei_device *device, const int threshold, double dx, double dy, bool is_discrete);
  void onPointerScrollEvent(ei_event *event);
  void onPointerScrollDiscreteEvent(ei_event *event);
  void onMotionEvent(ei_event *event);
  void onAbsMotionEvent(const ei_event *) const;
  bool onHotkey(KeyID key, bool isPressed, KeyModifierMask mask);
  void eiLogEvent(ei_log_priority priority, const char *message) const;

  void handleConnectedToEisEvent(const Event &event);
  void handlePortalSessionClosed();
  void ensureEmulating() const;
  void stopEmulating() const;
  EmulationSlot emulationSlotOf(const ei_device *device) const;
  void cancelIdleEmulationTimer() const;
  void updatePortalGlobalShortcuts();

  static void handleEiLogEvent(ei *ei, const ei_log_priority priority, const char *message, ei_log_context *)
  {
    auto computer = static_cast<EiComputer *>(ei_get_user_data(ei));
    computer->eiLogEvent(priority, message);
  }

private:
  // true if computer is being used as a primary computer, false otherwise
  bool m_isPrimary = false;
  IEventQueue *m_events = nullptr;

  // keyboard stuff
  EiKeyState *m_keyState = nullptr;

  KeyID m_lastPressed = kKeyNone;

  // mouse buttons currently held, indexed by ButtonID
  std::bitset<NumButtonIDs> m_buttons;

  // clipboard stuff
  EiClipboard *m_clipboard = nullptr;

  //! Direct Wayland selection access (ext-data-control), used when the compositor does not
  //! implement the portal Clipboard backend.  Only set on the primary computer.
  IClipboard *m_selectionClipboard = nullptr;
  size_t m_maximumClipboardSize = INT_MAX;

  std::vector<ei_device *> m_eiDevices;

  ei *m_ei = nullptr;
  ei_seat *m_eiSeat = nullptr;
  ei_device *m_eiPointer = nullptr;
  ei_device *m_eiKeyboard = nullptr;
  ei_device *m_eiAbs = nullptr;

  mutable std::uint32_t m_sequenceNumber = 0;

  // Lazily-started EIS emulation: only grab while relayed input is actually
  // flowing, and release after a short idle so the compositor can DPMS-sleep
  // this computer even while the deskflow cursor logically sits on it.
  // Tracked PER DEVICE: a pause of one device must not make the next input restart a device
  // EIS still holds in EMULATING (chk-n5va.49).
  mutable EmulationState m_emulating;
  //! Devices may only be told to emulate while EIS has them resumed.  Calling
  //! start_emulating on a paused or removed device makes EIS answer with
  //! "Invalid event from receiver ei context" and drop the client, which the portal
  //! then reports as a closed session - and libportal crashes on that teardown.
  mutable bool m_pointerResumed = false;
  mutable bool m_keyboardResumed = false;
  mutable bool m_absResumed = false;
  mutable EventQueueTimer *m_idleEmulationTimer = nullptr;
  // Chosen empirically on one machine in 2026-06; not derived from any protocol constant.
  static constexpr double s_idleEmulationTimeout = 4.0;

  std::uint32_t m_activeSides = 0;
  std::uint32_t m_x = 0;
  std::uint32_t m_y = 0;
  std::uint32_t m_w = 0;
  std::uint32_t m_h = 0;
  bool m_isShapeInitialized = false;

  // true if mouse has entered the computer
  bool m_isOnComputer;

  // server: last pointer position
  // client: position sent before enter()
  std::int32_t m_cursorX = 0;
  std::int32_t m_cursorY = 0;

  double m_bufferDX = 0;
  double m_bufferDY = 0;

  mutable std::mutex m_mutex;

  PortalRemoteDesktop *m_portalRemoteDesktop = nullptr;
  PortalInputCapture *m_portalInputCapture = nullptr;
  PortalGlobalShortcuts *m_portalGlobalShortcuts = nullptr;

  bool m_activated = false;

  struct HotKeyItem
  {
  public:
    HotKeyItem(std::uint32_t mask, std::uint32_t id);
    auto operator<=>(const HotKeyItem &other) const
    {
      return mask <=> other.mask;
    }

  public:
    std::uint32_t mask = 0;
    std::uint32_t id = 0; // for registering the hotkey
  };

  class HotKeySet
  {
  public:
    explicit HotKeySet(KeyID keyid);
    KeyID keyid() const
    {
      return m_id;
    }
    bool removeById(std::uint32_t id);
    void addItem(HotKeyItem item);
    std::uint32_t findByMask(std::uint32_t mask) const;
    bool empty() const
    {
      return m_set.empty();
    }
#ifdef HAVE_LIBPORTAL_SHORTCUTS
    const std::vector<PortalGlobalShortcuts::HotKey> getPortalHotKeys() const;
#endif

  private:
    KeyID m_id = 0;
    std::vector<HotKeyItem> m_set;
  };

  using HotKeyMap = std::map<KeyID, HotKeySet>;

  HotKeyMap m_hotkeys;
  [[no_unique_address]] XDGPowerManager m_powerManager;
};

} // namespace deskflow
