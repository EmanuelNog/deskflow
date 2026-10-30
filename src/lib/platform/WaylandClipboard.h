/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/ClipboardTypes.h"
#include "deskflow/IClipboard.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

struct wl_display;
struct wl_registry;
struct wl_seat;
struct ext_data_control_manager_v1;
struct ext_data_control_device_v1;
struct ext_data_control_source_v1;
struct ext_data_control_offer_v1;

namespace deskflow {

//! Wayland clipboard backed by the ext-data-control protocol
/*!
On Wayland the compositor owns the clipboard.  Two ways exist to reach it: the
xdg-desktop-portal Clipboard interface (portable, but several compositors do not
implement its backend - Hyprland among them) and the data-control protocols
(wlr-data-control, standardised as ext-data-control-v1), which is what
wl-clipboard uses.

This class implements the second path so clipboard sharing works where the portal
interface is missing.  It keeps a private wl_display connection on a worker thread
and mirrors the text and HTML selections in memory, which matches the IClipboard
model: poll getTime(), then read the formats.
*/
class WaylandClipboard : public IClipboard
{
public:
  using ChangeCallback = std::function<void()>;

  //! @param onChange invoked from the worker thread whenever the local selection changes
  explicit WaylandClipboard(ClipboardID id, ChangeCallback onChange = {});
  ~WaylandClipboard() override;

  //! True when the compositor offers ext-data-control-v1.
  static bool isAvailable();

  //! @name IClipboard overrides
  //@{
  bool empty() override;
  void add(Format format, const std::string &data) override;
  bool open(Time time) const override;
  void close() const override;
  Time getTime() const override;
  bool has(Format format) const override;
  std::string get(Format format) const override;
  //@}

  // Wayland callbacks (static members so they may touch private state)
  static void onRegistryGlobal(void *data, wl_registry *registry, uint32_t name, const char *iface, uint32_t version);
  static void onRegistryRemove(void *data, wl_registry *registry, uint32_t name);
  static void onDeviceDataOffer(void *data, ext_data_control_device_v1 *device, ext_data_control_offer_v1 *offer);
  static void onDeviceSelection(void *data, ext_data_control_device_v1 *device, ext_data_control_offer_v1 *offer);
  static void onDeviceFinished(void *data, ext_data_control_device_v1 *device);
  static void onDevicePrimarySelection(void *data, ext_data_control_device_v1 *device, ext_data_control_offer_v1 *offer);
  static void onSourceSend(void *data, ext_data_control_source_v1 *source, const char *mime, int32_t fd);
  static void onSourceCancelled(void *data, ext_data_control_source_v1 *source);

private:
  void start();
  void threadMain();
  void commitSelection();
  void handleSelection(ext_data_control_offer_v1 *offer);
  void processPendingSelection();
  void wake();

  ClipboardID m_id;
  ChangeCallback m_onChange;

  mutable std::mutex m_mutex;
  std::string m_text;
  std::string m_html;
  Time m_time = 0;
  bool m_ownSelection = false;

  //! data staged by empty()/add(), committed by close()
  mutable std::string m_pendingText;
  mutable std::string m_pendingHtml;
  mutable bool m_pendingCommit = false;

  std::thread m_thread;
  std::atomic<bool> m_running{false};
  int m_wakeFds[2] = {-1, -1};

  wl_display *m_display = nullptr;
  wl_registry *m_registry = nullptr;
  wl_seat *m_seat = nullptr;
  ext_data_control_manager_v1 *m_manager = nullptr;
  ext_data_control_device_v1 *m_device = nullptr;
  ext_data_control_source_v1 *m_source = nullptr;
  ext_data_control_offer_v1 *m_pendingOffer = nullptr;
};

} // namespace deskflow
