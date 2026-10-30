/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/WaylandClipboard.h"

#include "base/Log.h"

#include "protocols/ext-data-control-v1-client-protocol.h"

#include <wayland-client.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace deskflow {

namespace {

constexpr const char *kTextMime = "text/plain;charset=utf-8";
constexpr const char *kAltTextMime = "text/plain";
constexpr const char *kUtf8Mime = "UTF8_STRING";
constexpr const char *kHtmlMime = "text/html";

bool isTextMime(const char *mime)
{
  if (mime == nullptr) {
    return false;
  }
  return strcmp(mime, kTextMime) == 0 || strcmp(mime, kAltTextMime) == 0 || strcmp(mime, kUtf8Mime) == 0 ||
         strcmp(mime, "STRING") == 0 || strcmp(mime, "TEXT") == 0;
}

std::string readAll(int fd)
{
  std::string out;
  char buffer[8192];
  ssize_t n = 0;
  while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
    out.append(buffer, static_cast<size_t>(n));
  }
  return out;
}

bool writeAll(int fd, const std::string &data)
{
  size_t offset = 0;
  while (offset < data.size()) {
    const ssize_t n = write(fd, data.data() + offset, data.size() - offset);
    if (n <= 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    offset += static_cast<size_t>(n);
  }
  return true;
}

//! What the current selection offers; filled by the offer listener.
struct OfferMimes
{
  bool text = false;
  bool html = false;
};

void onOfferMime(void *data, ext_data_control_offer_v1 *offer, const char *mime)
{
  (void)offer;
  auto *mimes = static_cast<OfferMimes *>(data);
  if (isTextMime(mime)) {
    mimes->text = true;
  } else if (mime != nullptr && strcmp(mime, kHtmlMime) == 0) {
    mimes->html = true;
  }
}

const ext_data_control_offer_v1_listener kOfferListener = {
    /* .offer = */ onOfferMime,
};

const wl_registry_listener kRegistryListener = {
    /* .global = */ WaylandClipboard::onRegistryGlobal,
    /* .global_remove = */ WaylandClipboard::onRegistryRemove,
};

const ext_data_control_device_v1_listener kDeviceListener = {
    /* .data_offer = */ WaylandClipboard::onDeviceDataOffer,
    /* .selection = */ WaylandClipboard::onDeviceSelection,
    /* .finished = */ WaylandClipboard::onDeviceFinished,
    /* .primary_selection = */ WaylandClipboard::onDevicePrimarySelection,
};

const ext_data_control_source_v1_listener kSourceListener = {
    /* .send = */ WaylandClipboard::onSourceSend,
    /* .cancelled = */ WaylandClipboard::onSourceCancelled,
};

} // namespace

// ---------------------------------------------------------------- lifecycle

WaylandClipboard::WaylandClipboard(ClipboardID id, ChangeCallback onChange)
    : m_id(id), m_onChange(std::move(onChange))
{
  start();
}

WaylandClipboard::~WaylandClipboard()
{
  m_running = false;
  wake();
  if (m_thread.joinable()) {
    m_thread.join();
  }
  if (m_display != nullptr) {
    wl_display_disconnect(m_display);
    m_display = nullptr;
  }
  if (m_wakeFds[0] >= 0) {
    ::close(m_wakeFds[0]);
    ::close(m_wakeFds[1]);
  }
}

bool WaylandClipboard::isAvailable()
{
  wl_display *display = wl_display_connect(nullptr);
  if (display == nullptr) {
    return false;
  }
  bool found = false;
  wl_registry *registry = wl_display_get_registry(display);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - C API
  wl_registry_listener listener = {
      .global =
          [](void *data, wl_registry *, uint32_t, const char *iface, uint32_t) {
            if (iface != nullptr && strcmp(iface, ext_data_control_manager_v1_interface.name) == 0) {
              *static_cast<bool *>(data) = true;
            }
          },
      .global_remove = [](void *, wl_registry *, uint32_t) {},
  };
  wl_registry_add_listener(registry, &listener, &found);
  wl_display_roundtrip(display);
  wl_display_disconnect(display);
  return found;
}

void WaylandClipboard::start()
{
  m_display = wl_display_connect(nullptr);
  if (m_display == nullptr) {
    LOG_WARN("wayland clipboard: cannot connect to the compositor");
    return;
  }
  if (pipe(m_wakeFds) != 0) {
    LOG_WARN("wayland clipboard: cannot create wake pipe: %s", strerror(errno));
    m_wakeFds[0] = m_wakeFds[1] = -1;
    return;
  }
  for (int fd : m_wakeFds) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
  }

  m_running = true;
  m_thread = std::thread([this] { threadMain(); });
  LOG_DEBUG("wayland clipboard: started on ext-data-control-v1");
}

void WaylandClipboard::threadMain()
{
  m_registry = wl_display_get_registry(m_display);
  wl_registry_add_listener(m_registry, &kRegistryListener, this);
  wl_display_roundtrip(m_display); // globals
  if (m_manager != nullptr && m_seat != nullptr) {
    m_device = ext_data_control_manager_v1_get_data_device(m_manager, m_seat);
    ext_data_control_device_v1_add_listener(m_device, &kDeviceListener, this);
    wl_display_roundtrip(m_display); // initial selection
  } else {
    LOG_WARN("wayland clipboard: compositor has no ext-data-control manager or no seat");
  }

  while (m_running) {
    while (wl_display_prepare_read(m_display) != 0) {
      wl_display_dispatch_pending(m_display);
    }
    wl_display_flush(m_display);

    pollfd fds[2] = {
        {.fd = wl_display_get_fd(m_display), .events = POLLIN, .revents = 0},
        {.fd = m_wakeFds[0], .events = POLLIN, .revents = 0},
    };
    const int rc = poll(fds, 2, 1000);
    if (rc < 0) {
      wl_display_cancel_read(m_display);
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if ((fds[1].revents & POLLIN) != 0) {
      wl_display_cancel_read(m_display);
      char drain[64];
      while (read(m_wakeFds[0], drain, sizeof(drain)) > 0) { }
      commitSelection();
      wl_display_flush(m_display);
      continue;
    }
    if ((fds[0].revents & POLLIN) != 0) {
      if (wl_display_read_events(m_display) != 0) {
        break;
      }
      wl_display_dispatch_pending(m_display);
    } else {
      wl_display_cancel_read(m_display);
    }
    processPendingSelection();
  }
}

void WaylandClipboard::wake()
{
  if (m_wakeFds[1] >= 0) {
    const char byte = 'w';
    // NOLINTNEXTLINE(bugprone-unused-return-value) - best effort wakeup
    (void)!write(m_wakeFds[1], &byte, 1);
  }
}

// ---------------------------------------------------------------- wayland callbacks

void WaylandClipboard::onRegistryGlobal(
    void *data, wl_registry *registry, uint32_t name, const char *iface, uint32_t version
)
{
  auto *self = static_cast<WaylandClipboard *>(data);
  if (iface == nullptr) {
    return;
  }
  if (strcmp(iface, ext_data_control_manager_v1_interface.name) == 0) {
    self->m_manager = static_cast<ext_data_control_manager_v1 *>(
        wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, version < 2 ? version : 2)
    );
  } else if (strcmp(iface, wl_seat_interface.name) == 0 && self->m_seat == nullptr) {
    self->m_seat =
        static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, version < 7 ? version : 7));
  }
}

void WaylandClipboard::onRegistryRemove(void *data, wl_registry *registry, uint32_t name)
{
  (void)data;
  (void)registry;
  (void)name;
}

void WaylandClipboard::onDeviceDataOffer(
    void *data, ext_data_control_device_v1 *device, ext_data_control_offer_v1 *offer
)
{
  (void)data;
  (void)device;
  if (offer == nullptr) {
    return;
  }
  // The mime types arrive as offer events right after the offer itself, so collect them
  // asynchronously and read the selection once the batch has been dispatched.
  auto *mimes = new OfferMimes();
  ext_data_control_offer_v1_add_listener(offer, &kOfferListener, mimes);
  wl_proxy_set_user_data(reinterpret_cast<wl_proxy *>(offer), mimes);
}

void WaylandClipboard::onDeviceSelection(
    void *data, ext_data_control_device_v1 *device, ext_data_control_offer_v1 *offer
)
{
  (void)device;
  static_cast<WaylandClipboard *>(data)->handleSelection(offer);
}

//! Primary selection is not shared; the event must still have a handler, because libwayland
//! aborts when a listener entry is NULL.
void WaylandClipboard::onDevicePrimarySelection(
    void *data, ext_data_control_device_v1 *device, ext_data_control_offer_v1 *offer
)
{
  (void)data;
  (void)device;
  if (offer == nullptr) {
    return;
  }
  auto *mimes = static_cast<OfferMimes *>(wl_proxy_get_user_data(reinterpret_cast<wl_proxy *>(offer)));
  ext_data_control_offer_v1_destroy(offer);
  delete mimes;
}

void WaylandClipboard::onDeviceFinished(void *data, ext_data_control_device_v1 *device)
{
  (void)data;
  if (device != nullptr) {
    ext_data_control_device_v1_destroy(device);
  }
}

void WaylandClipboard::handleSelection(ext_data_control_offer_v1 *offer)
{
  if (offer == nullptr) {
    // selection cleared
    {
      std::scoped_lock lock{m_mutex};
      if (!m_text.empty() || !m_html.empty()) {
        m_text.clear();
        m_html.clear();
        ++m_time;
      }
    }
    return;
  }
  if (m_pendingOffer != nullptr) {
    // superseded before we read it
    auto *stale = static_cast<OfferMimes *>(wl_proxy_get_user_data(reinterpret_cast<wl_proxy *>(m_pendingOffer)));
    ext_data_control_offer_v1_destroy(m_pendingOffer);
    delete stale;
  }
  m_pendingOffer = offer;
}

void WaylandClipboard::processPendingSelection()
{
  ext_data_control_offer_v1 *offer = m_pendingOffer;
  if (offer == nullptr) {
    return;
  }
  m_pendingOffer = nullptr;

  auto *mimes = static_cast<OfferMimes *>(wl_proxy_get_user_data(reinterpret_cast<wl_proxy *>(offer)));
  if (mimes == nullptr) {
    ext_data_control_offer_v1_destroy(offer);
    return;
  }
  // Outside the event callback now, so a roundtrip is safe and the offer events land.
  wl_display_roundtrip(m_display);

  std::string text;
  std::string html;
  if (mimes->text) {
    int fds[2] = {-1, -1};
    if (pipe(fds) == 0) {
      ext_data_control_offer_v1_receive(offer, kTextMime, fds[1]);
      ::close(fds[1]);
      wl_display_flush(m_display);
      text = readAll(fds[0]);
      ::close(fds[0]);
    }
  }
  if (mimes->html) {
    int fds[2] = {-1, -1};
    if (pipe(fds) == 0) {
      ext_data_control_offer_v1_receive(offer, kHtmlMime, fds[1]);
      ::close(fds[1]);
      wl_display_flush(m_display);
      html = readAll(fds[0]);
      ::close(fds[0]);
    }
  }
  ext_data_control_offer_v1_destroy(offer);
  delete mimes;

  {
    std::scoped_lock lock{m_mutex};
    if (text == m_text && html == m_html) {
      return;
    }
    m_text = std::move(text);
    m_html = std::move(html);
    m_ownSelection = false;
    ++m_time;
  }
  LOG_DEBUG("wayland clipboard: local selection changed (%zu text bytes, %zu html bytes)", m_text.size(), m_html.size());
  if (m_onChange) {
    m_onChange();
  }
}

void WaylandClipboard::commitSelection()
{
  std::string text;
  std::string html;
  {
    std::scoped_lock lock{m_mutex};
    if (!m_pendingCommit) {
      return;
    }
    m_pendingCommit = false;
    text = m_pendingText;
    html = m_pendingHtml;
    m_text = text;
    m_html = html;
    m_ownSelection = true;
    ++m_time;
  }

  if (m_manager == nullptr || m_device == nullptr) {
    return;
  }
  if (m_source != nullptr) {
    ext_data_control_source_v1_destroy(m_source);
    m_source = nullptr;
  }
  m_source = ext_data_control_manager_v1_create_data_source(m_manager);
  ext_data_control_source_v1_add_listener(m_source, &kSourceListener, this);
  if (!text.empty()) {
    ext_data_control_source_v1_offer(m_source, kTextMime);
  }
  if (!html.empty()) {
    ext_data_control_source_v1_offer(m_source, kHtmlMime);
  }
  ext_data_control_device_v1_set_selection(m_device, m_source);
  LOG_DEBUG("wayland clipboard: published selection (%zu text bytes, %zu html bytes)", text.size(), html.size());
}

void WaylandClipboard::onSourceSend(
    void *data, ext_data_control_source_v1 *source, const char *mime, int32_t fd
)
{
  (void)source;
  auto *self = static_cast<WaylandClipboard *>(data);
  std::string payload;
  {
    std::scoped_lock lock{self->m_mutex};
    payload = isTextMime(mime) ? self->m_text : self->m_html;
  }
  if (!writeAll(fd, payload)) {
    LOG_DEBUG("wayland clipboard: failed to serve selection data: %s", strerror(errno));
  }
  ::close(fd);
}

void WaylandClipboard::onSourceCancelled(void *data, ext_data_control_source_v1 *source)
{
  (void)source;
  auto *self = static_cast<WaylandClipboard *>(data);
  {
    std::scoped_lock lock{self->m_mutex};
    self->m_ownSelection = false;
  }
  LOG_DEBUG("wayland clipboard: selection ownership lost");
}

// ---------------------------------------------------------------- IClipboard

bool WaylandClipboard::empty()
{
  std::scoped_lock lock{m_mutex};
  m_pendingText.clear();
  m_pendingHtml.clear();
  return true;
}

void WaylandClipboard::add(Format format, const std::string &data)
{
  std::scoped_lock lock{m_mutex};
  switch (format) {
  case Format::Text:
    m_pendingText = data;
    m_pendingAdded = true;
    break;
  case Format::HTML:
    m_pendingHtml = data;
    m_pendingAdded = true;
    break;
  default:
    break; // bitmaps are not supported by this backend (yet)
  }
}

bool WaylandClipboard::open(Time time) const
{
  (void)time;
  return true;
}

void WaylandClipboard::close() const
{
  {
    std::scoped_lock lock{m_mutex};
    if (!m_pendingAdded) {
      return; // read-only use (IClipboard::copy closes its source): nothing to publish
    }
    m_pendingAdded = false;
    m_pendingCommit = true;
  }
  const_cast<WaylandClipboard *>(this)->wake();
}

WaylandClipboard::Time WaylandClipboard::getTime() const
{
  std::scoped_lock lock{m_mutex};
  return m_time;
}

bool WaylandClipboard::has(Format format) const
{
  std::scoped_lock lock{m_mutex};
  switch (format) {
  case Format::Text:
    return !m_text.empty();
  case Format::HTML:
    return !m_html.empty();
  default:
    return false;
  }
}

std::string WaylandClipboard::get(Format format) const
{
  std::scoped_lock lock{m_mutex};
  switch (format) {
  case Format::Text:
    return m_text;
  case Format::HTML:
    return m_html;
  default:
    return {};
  }
}

} // namespace deskflow
