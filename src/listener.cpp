/*
 *  Copyright (c) 2019 by flomesh.io
 *
 *  Unless prior written consent has been obtained from the copyright
 *  owner, the following shall not be allowed.
 *
 *  1. The distribution of any source codes, header files, make files,
 *     or libraries of the software.
 *
 *  2. Disclosure of any source codes pertaining to the software to any
 *     additional parties.
 *
 *  3. Alteration or removal of any notices in or on the software or
 *     within the documentation included within the software.
 *
 *  ALL SOURCE CODE AS WELL AS ALL DOCUMENTATION INCLUDED WITH THIS
 *  SOFTWARE IS PROVIDED IN AN “AS IS” CONDITION, WITHOUT WARRANTY OF ANY
 *  KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 *  OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 *  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 *  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 *  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 *  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "listener.hpp"
#include "pipeline.hpp"
#include "logging.hpp"

#include <set>

namespace pipy {

using tcp = asio::ip::tcp;

//
// Listener::Options
//

Listener::Options::Options(pjs::Object *options) {
  Value(options, "maxConnections")
    .get(max_connections)
    .check_nullable();
  Value(options, "readTimeout")
    .get_seconds(read_timeout)
    .check_nullable();
  Value(options, "writeTimeout")
    .get_seconds(write_timeout)
    .check_nullable();
  Value(options, "idleTimeout")
    .get_seconds(idle_timeout)
    .check_nullable();
  Value(options, "transparent")
    .get(transparent)
    .check_nullable();
  Value(options, "closeEOF")
    .get(close_eof)
    .check_nullable();
}

//
// Listener
//

bool Listener::s_reuse_port = false;

std::list<Listener*> Listener::s_all_listeners;

void Listener::set_reuse_port(bool reuse) {
  s_reuse_port = reuse;
}

Listener::Listener(const std::string &ip, int port)
  : m_ip(ip)
  , m_port(port)
  , m_acceptor(Net::context())
{
  m_address = asio::ip::make_address(m_ip);
  m_ip = m_address.to_string();
  s_all_listeners.push_back(this);
}

Listener::~Listener() {
  s_all_listeners.remove(this);
}

void Listener::pipeline_layout(PipelineLayout *layout) {
  if (m_pipeline_layout.get() != layout) {
    if (layout) {
      if (!m_pipeline_layout) {
        try {
          start();
        } catch (std::runtime_error &err) {
          m_acceptor.close();
          throw err;
        }
      }
    } else if (m_pipeline_layout) {
      close();
    }
    m_pipeline_layout = layout;
  }
}

void Listener::close() {
  m_acceptor.close();
  Log::info("[listener] Stopped listening on port %d at %s", m_port, m_ip.c_str());
}

void Listener::set_options(const Options &options) {
  m_options = options;
  if (m_pipeline_layout) {
    int n = m_options.max_connections;
    if (n >= 0 && m_inbounds.size() >= n) {
      pause();
    } else {
      resume();
    }
  }
}

void Listener::start() {
  try {
    tcp::endpoint endpoint(m_address, m_port);
    m_acceptor.open(endpoint.protocol());
    m_acceptor.set_option(asio::socket_base::reuse_address(true));

    auto sock = m_acceptor.native_handle();

#ifdef __linux__
    if (m_options.transparent) {
      int enabled = 1;
      setsockopt(sock, SOL_IP, IP_TRANSPARENT, &enabled, sizeof(enabled));
    }
#endif

    if (s_reuse_port) {
      int enabled = 1;
#ifdef __FreeBSD__
      setsockopt(sock, SOL_SOCKET, SO_REUSEPORT_LB, &enabled, sizeof(enabled));
#else
      setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled));
#endif
    }

    m_acceptor.bind(endpoint);
    m_acceptor.listen(asio::socket_base::max_connections);

    if (m_options.max_connections < 0 || m_inbounds.size() < m_options.max_connections) {
      accept();
    }

    Log::info("[listener] Listening on port %d at %s", m_port, m_ip.c_str());

  } catch (std::runtime_error &err) {
    char msg[200];
    std::snprintf(
      msg, sizeof(msg),
      "[listener] Cannot start listening on port %d at %s: ",
      m_port, m_ip.c_str()
    );
    throw std::runtime_error(std::string(msg) + err.what());
  }
}

void Listener::accept() {
  auto inbound = InboundTCP::make(this, m_options);
  inbound->accept(m_acceptor);
}

void Listener::pause() {
  if (!m_paused) {
    m_acceptor.cancel();
    m_paused = true;
  }
}

void Listener::resume() {
  if (m_paused) {
    accept();
    m_paused = false;
  }
}

void Listener::open(InboundTCP *inbound) {
  m_inbounds.push(inbound);
  m_peak_connections = std::max(m_peak_connections, int(m_inbounds.size()));
  int n = m_options.max_connections;
  if (n > 0 && m_inbounds.size() >= n) {
    pause();
  } else {
    accept();
  }
}

void Listener::close(InboundTCP *inbound) {
  m_inbounds.remove(inbound);
  int n = m_options.max_connections;
  if (n < 0 || m_inbounds.size() < n) {
    resume();
  }
}

auto Listener::find(const std::string &ip, int port) -> Listener* {
  auto addr = asio::ip::make_address(ip);
  for (auto *l : s_all_listeners) {
    if (l->m_port == port && l->m_address == addr) {
      return l;
    }
  }
  return nullptr;
}

} // namespace pipy
