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

#include "mux.hpp"
#include "pipeline.hpp"
#include "input.hpp"
#include "utils.hpp"
#include "log.hpp"

#include <limits>

namespace pipy {

//
// MuxBase::Options
//

MuxBase::Options::Options(pjs::Object *options) {
  thread_local static pjs::ConstStr s_max_idle("maxIdle");
  thread_local static pjs::ConstStr s_max_queue("maxQueue");
  thread_local static pjs::ConstStr s_max_messages("maxMessages");
  Value(options, s_max_idle)
    .get_seconds(max_idle)
    .check_nullable();
  Value(options, s_max_queue)
    .get(max_queue)
    .check_nullable();
  Value(options, s_max_messages)
    .get(max_messages)
    .check_nullable();
}

//
// MuxBase
//
// This is the base implementation for all mux filters.
//
// - On arrival of the very first event:
//     1. Call the session selector provided by user to get a session key
//     2. Allocate a Session with the requested session key
// - On arrival of messages:
//     1. Create a Stream from the selected Session if haven't yet and pass the first message to it
//     2. Buffer up all following messages if the current Stream hasn't outputted StreamEnd yet
// - On arrival of a StreamEnd from the current Stream:
//     1. Close the current Stream
//     2. Create a new Stream and pass the next message to it if there is one in the buffer
// - On filter reset
//     1. Close the current Stream if any
//     2. Free the selected Session
//

MuxBase::MuxBase()
  : m_session_pool(new SessionPool())
{
}

MuxBase::MuxBase(const MuxBase &r)
  : m_session_pool(r.m_session_pool)
{
}

void MuxBase::reset() {
  if (m_session) {
    stop_waiting();
    if (m_stream) {
      m_stream->chain(nullptr);
      m_session->close_stream(m_stream);
      m_stream = nullptr;
    }
    m_session->free();
    m_session = nullptr;
  }
  m_waiting_events.clear();
  m_session_key = pjs::Value::undefined;
}

void MuxBase::shutdown() {
  m_session_pool->shutdown();
}

void MuxBase::open_stream(EventTarget::Input *output) {
  if (!m_stream) {
    auto session = m_session.get();
    if (!session) {
      if (!on_select_session(m_session_key)) return;
      session = m_session_pool->alloc(this, m_session_key);
      if (!session) return;
      m_session = session;
    }

    if (!session->m_pipeline) {
      pjs::Value args[2];
      args[0] = m_session_key;
      args[1].set((int)session->m_cluster->m_sessions.size());
      auto p = on_new_pipeline(session->reply(), args);
      session->link(p);
    }

    if (session->is_pending()) {
      start_waiting();
      return;
    }

    auto s = m_session->open_stream();
    s->chain(output);
    m_stream = s;
  }
}

void MuxBase::write_stream(Event *evt) {
  if (m_waiting) {
    m_waiting_events.push(evt);
  } else if (m_stream) {
    m_stream->input()->input(evt);
  }
}

void MuxBase::start_waiting() {
  if (!m_waiting) {
    m_session->m_waiting_muxers.push(this);
    m_waiting = true;
  }
}

void MuxBase::flush_waiting() {
  on_pending_session_open();
  stop_waiting();
}

void MuxBase::stop_waiting() {
  if (m_waiting) {
    m_session->m_waiting_muxers.remove(this);
    m_waiting = false;
  }
}

//
// MuxBase::Session
//
// Construction:
//   - When a new session key is requested by MuxBase
//
// Destruction:
//   - When share count is 0 for a time of maxIdle
//   - When freed by MuxBase if it is detached from its SessionCluster
//
// Session owns streams:
//   - Session::open_stream(): Creates a new stream
//   - Session::close_stream(): Destroys an existing stream
//

void MuxBase::Session::detach() {
  if (auto cluster = m_cluster) {
    m_cluster = nullptr;
    cluster->discard(this);
  }
}

void MuxBase::Session::set_pending(bool pending) {
  if (pending != m_is_pending) {
    if (!pending) {
      for (auto *p = m_waiting_muxers.head(); p; ) {
        auto *muxer = p; p = p->List<MuxBase>::Item::next();
        muxer->flush_waiting();
      }
    }
    m_is_pending = pending;
  }
}

void MuxBase::Session::link(Pipeline *pipeline) {
  m_pipeline = pipeline;
  chain_forward(pipeline->input());
  open();
}

void MuxBase::Session::unlink() {
  if (auto p = m_pipeline.get()) {
    close();
    forward(StreamEnd::make());
    Pipeline::auto_release(p);
    m_pipeline = nullptr;
  }
}

void MuxBase::Session::free() {
  if (m_cluster) {
    m_cluster->free(this);
  } else {
    unlink();
  }
}

void MuxBase::Session::on_input(Event *evt) {
  forward(evt);
}

void MuxBase::Session::on_reply(Event *evt) {
  output(evt);
  if (evt->is<StreamEnd>()) {
    m_is_closed = true;
  }
}

//
// MuxBase::SessionCluster
//
// This is the container for all Sessions with the same session key.
//

MuxBase::SessionCluster::SessionCluster(MuxBase *mux, const Options &options) {
  m_max_idle = options.max_idle;
  m_max_queue = options.max_queue;
  m_max_messages = options.max_messages;
}

auto MuxBase::SessionCluster::alloc() -> Session* {
  auto max_share_count = m_max_queue;
  auto max_message_count = m_max_messages;
  auto *s = m_sessions.head();
  while (s) {
    if (!s->m_is_closed) {
      if ((max_share_count <= 0 || s->m_share_count < max_share_count) &&
          (max_message_count <= 0 || s->m_message_count < max_message_count)
       ) {
        s->m_share_count++;
        s->m_message_count++;
        sort(s);
        return s;
      }
    }
    s = s->next();
  }
  s = session();
  s->m_cluster = this;
  s->retain();
  m_sessions.unshift(s);
  return s;
}

void MuxBase::SessionCluster::free(Session *session) {
  session->m_share_count--;
  if (session->is_free()) {
    session->m_free_time = utils::now();
  }
  sort(session);
}

void MuxBase::SessionCluster::discard(Session *session) {
  Session::auto_release(session);
  m_sessions.remove(session);
  session->release();
  sort(nullptr);
}

void MuxBase::SessionCluster::sort(Session *session) {
  if (session) {
    auto p = session->back();
    while (p && p->m_share_count > session->m_share_count) p = p->back();
    if (p == session->back()) {
      auto p = session->next();
      while (p && p->m_share_count < session->m_share_count) p = p->next();
      if (p != session->next()) {
        m_sessions.remove(session);
        if (p) {
          m_sessions.insert(session, p);
        } else {
          m_sessions.push(session);
        }
      }
    } else {
      m_sessions.remove(session);
      if (p) {
        m_sessions.insert(session, p->next());
      } else {
        m_sessions.unshift(session);
      }
    }
  }

  schedule_recycling();

  if (m_sessions.empty()) {
    if (m_weak_key.original_ptr()) {
      m_pool->m_weak_clusters.erase(m_weak_key);
    } else {
      m_pool->m_clusters.erase(m_key);
    }
    free();
  }
}

void MuxBase::SessionCluster::schedule_recycling() {
  auto s = m_sessions.head();
  if (!s || s->m_share_count > 0) {
    if (m_recycle_scheduled) {
      m_pool->m_recycle_clusters.remove(this);
      m_recycle_scheduled = false;
    }
  } else {
    if (!m_recycle_scheduled) {
      m_pool->m_recycle_clusters.push(this);
      m_pool->recycle();
      m_recycle_scheduled = true;
    }
  }
}

void MuxBase::SessionCluster::recycle(double now) {
  auto max_idle = m_max_idle * 1000;
  auto s = m_sessions.head();
  while (s) {
    auto session = s; s = s->next();
    if (session->m_share_count > 0) break;
    if (session->m_is_closed || m_weak_ptr_gone ||
       (m_max_messages > 0 && session->m_message_count >= m_max_messages) ||
       (now - session->m_free_time >= max_idle))
    {
      session->unlink();
      session->detach();
    }
  }
}

void MuxBase::SessionCluster::on_weak_ptr_gone() {
  m_weak_ptr_gone = true;
  m_pool->m_weak_clusters.erase(m_weak_key);
  schedule_recycling();
}

//
// MuxBase::SessionPool
//

MuxBase::SessionPool::~SessionPool() {
  for (const auto &p : m_clusters) p.second->free();
  for (const auto &p : m_weak_clusters) p.second->free();
}

auto MuxBase::SessionPool::alloc(MuxBase *mux, const pjs::Value &key) -> Session* {
  bool is_weak = (key.is_object() && key.o());
  SessionCluster *cluster = nullptr;

  if (is_weak) {
    pjs::WeakRef<pjs::Object> o(key.o());
    auto i = m_weak_clusters.find(o);
    if (i != m_weak_clusters.end()) {
      cluster = i->second;
    }
  } else {
    auto i = m_clusters.find(key);
    if (i != m_clusters.end()) {
      cluster = i->second;
    }
  }

  if (cluster) return cluster->alloc();

  cluster = mux->on_new_cluster();
  if (!cluster) return nullptr;

  cluster->m_pool = this;

  if (is_weak) {
    cluster->m_weak_key = key.o();
    cluster->watch(key.o()->weak_ptr());
    m_weak_clusters[key.o()] = cluster;
  } else {
    cluster->m_key = key;
    m_clusters[key] = cluster;
  }

  return cluster->alloc();
}

void MuxBase::SessionPool::shutdown() {
  m_has_shutdown = true;
}

void MuxBase::SessionPool::recycle() {
  if (m_recycling) return;
  if (m_recycle_clusters.empty()) return;

  m_recycle_timer.schedule(
    1.0,
    [this]() {
      InputContext ic;
      m_recycling = false;
      auto now = m_has_shutdown ? std::numeric_limits<double>::infinity() : utils::now();
      auto c = m_recycle_clusters.head();
      while (c) {
        auto cluster = c; c = c->next();
        cluster->recycle(now);
      }
      recycle();
      release();
    }
  );

  retain();
  m_recycling = true;
}

//
// QueueMuxer
//

void QueueMuxer::reset() {
  while (auto s = m_streams.head()) {
    m_streams.remove(s);
    s->release();
  }
  m_dedicated = false;
}

auto QueueMuxer::open_stream() -> EventFunction* {
  auto s = new Stream(this);
  s->retain();
  return s;
}

void QueueMuxer::close_stream(EventFunction *stream) {
  auto s = static_cast<Stream*>(stream);
  s->release();
}

void QueueMuxer::set_one_way(EventFunction *stream) {
  auto s = static_cast<Stream*>(stream);
  s->m_one_way = true;
}

void QueueMuxer::increase_queue_count() {
  if (auto s = m_streams.head()) {
    s->m_queued_count++;
  }
}

void QueueMuxer::dedicate() {
  m_dedicated = true;
}

void QueueMuxer::on_reply(Event *evt) {
  if (m_dedicated) {
    if (auto s = m_streams.head()) {
      s->m_dedicated = true;
      s->output(evt);
    }
    return;
  }

  if (evt->is<MessageStart>()) {
    if (auto s = m_streams.head()) {
      if (!s->m_started) {
        s->output(evt);
        s->m_started = true;
      }
    }

  } else if (evt->is<Data>()) {
    if (auto s = m_streams.head()) {
      if (s->m_started) {
        s->output(evt);
      }
    }

  } else if (evt->is<MessageEnd>()) {
    if (auto s = m_streams.head()) {
      if (s->m_started) {
        if (!--s->m_queued_count) {
          m_streams.remove(s);
          s->output(evt);
          s->release();
        } else {
          s->m_started = false;
          s->output(evt);
        }
      }
    }

  } else if (evt->is<StreamEnd>()) {
    while (auto s = m_streams.head()) {
      m_streams.remove(s);
      if (!s->m_started) {
        s->output(MessageStart::make());
      }
      s->output(evt->clone());
      s->release();
    }
  }
}

//
// QueueMuxer::Stream
//
// Retain:
//   - Session::open_stream()
//   - After queued
// Release:
//   - Session::close_stream()
//   - After replied
//

void QueueMuxer::Stream::on_event(Event *evt) {
  auto muxer = m_muxer;

  if (m_dedicated) {
    muxer->output(evt);
    return;
  }

  if (auto start = evt->as<MessageStart>()) {
    if (!m_start) {
      m_start = start;
    }

  } else if (auto data = evt->as<Data>()) {
    if (m_start && !m_queued_count) {
      m_buffer.push(*data);
    }

  } else if (evt->is<MessageEnd>() || evt->is<StreamEnd>()) {
    if (m_start && !m_queued_count) {
      m_queued_count = 1;
      if (!m_one_way) {
        muxer->m_streams.push(this);
        retain();
      }
      auto *end = evt->as<MessageEnd>();
      muxer->output(m_start);
      if (!m_buffer.empty()) {
        muxer->output(Data::make(std::move(m_buffer)));
      }
      muxer->output(end ? end : MessageEnd::make());
    }
  }
}

//
// MuxQueue::Options
//

MuxQueue::Options::Options(pjs::Object *options)
  : MuxBase::Options(options)
{
  Value(options, "isOneWay")
    .get(is_one_way)
    .check_nullable();
}

//
// MuxQueue
//

MuxQueue::MuxQueue()
{
}

MuxQueue::MuxQueue(pjs::Function *session_selector)
  : m_session_selector(session_selector)
{
}

MuxQueue::MuxQueue(pjs::Function *session_selector, const Options &options)
  : m_session_selector(session_selector)
  , m_options(options)
{
}

MuxQueue::MuxQueue(pjs::Function *session_selector, pjs::Function *options)
  : m_session_selector(session_selector)
  , m_options_f(options)
{
}

MuxQueue::MuxQueue(const MuxQueue &r)
  : Filter(r)
  , MuxBase(r)
  , m_session_selector(r.m_session_selector)
  , m_options(r.m_options)
  , m_options_f(r.m_options_f)
{
}

MuxQueue::~MuxQueue() {
}

void MuxQueue::dump(Dump &d) {
  Filter::dump(d);
  d.name = "muxQueue";
  d.sub_type = Dump::MUX;
}

auto MuxQueue::clone() -> Filter* {
  return new MuxQueue(*this);
}

void MuxQueue::reset() {
  Filter::reset();
  MuxBase::reset();
  m_started = false;
}

void MuxQueue::process(Event *evt) {
  MuxBase::open_stream(Filter::output());
  MuxBase::write_stream(evt);

  if (auto *f = m_options.is_one_way.get()) {
    if (!m_started) {
      if (auto *start = evt->as<MessageStart>()) {
        if (auto *s = MuxBase::stream()) {
          pjs::Value arg(start), ret;
          if (Filter::callback(f, 1, &arg, ret)) {
            if (ret.to_boolean()) {
              auto *session = static_cast<Session*>(MuxBase::session());
              static_cast<QueueMuxer*>(session)->set_one_way(s);
            }
          }
        }
        m_started = true;
      }
    }
  }
}

bool MuxQueue::on_select_session(pjs::Value &key) {
  if (m_session_selector && !eval(m_session_selector, key)) return false;
  if (key.is_undefined()) key.set(Filter::context()->inbound());
  return true;
}

auto MuxQueue::on_new_cluster() -> MuxBase::SessionCluster* {
  if (auto f = m_options_f.get()) {
    pjs::Value opts;
    if (!Filter::eval(f, opts)) return nullptr;
    if (!opts.is_object()) {
      Filter::error("callback did not return an object for options");
      return nullptr;
    }
    try {
      Options options(opts.o());
      return new SessionCluster(this, options);
    } catch (std::runtime_error &err) {
      Filter::error(err.what());
      return nullptr;
    }
  } else {
    return new SessionCluster(this, m_options);
  }
}

auto MuxQueue::on_new_pipeline(EventTarget::Input *output, pjs::Value args[2]) -> Pipeline* {
  return Filter::sub_pipeline(0, true, output, nullptr, 2, args);
}

//
// MuxQueue::Session
//

void MuxQueue::Session::open() {
  QueueMuxer::chain(MuxBase::Session::input());
  MuxBase::Session::chain(QueueMuxer::reply());
}

auto MuxQueue::Session::open_stream() -> EventFunction* {
  return QueueMuxer::open_stream();
}

void MuxQueue::Session::close_stream(EventFunction *stream) {
  return QueueMuxer::close_stream(stream);
}

void MuxQueue::Session::close() {
  QueueMuxer::reset();
}

//
// Mux
//

Mux::Mux()
{
}

Mux::Mux(pjs::Function *session_selector)
  : m_session_selector(session_selector)
{
}

Mux::Mux(pjs::Function *session_selector, const Options &options)
  : m_session_selector(session_selector)
  , m_options(options)
{
}

Mux::Mux(pjs::Function *session_selector, pjs::Function *options)
  : m_session_selector(session_selector)
  , m_options_f(options)
{
}

Mux::Mux(const Mux &r)
  : Filter(r)
  , MuxBase(r)
  , m_session_selector(r.m_session_selector)
  , m_options(r.m_options)
  , m_options_f(r.m_options_f)
{
}

Mux::~Mux()
{
}

void Mux::dump(Dump &d) {
  Filter::dump(d);
  d.name = "mux";
  d.sub_type = Dump::MUX;
  d.out_type = Dump::OUTPUT_FROM_SELF;
}

auto Mux::clone() -> Filter* {
  return new Mux(*this);
}

void Mux::reset() {
  Filter::reset();
  MuxBase::reset();
}

void Mux::process(Event *evt) {
  MuxBase::open_stream(Filter::output());
  MuxBase::write_stream(evt);
  output(evt);
}

bool Mux::on_select_session(pjs::Value &key) {
  if (m_session_selector && !eval(m_session_selector, key)) return false;
  if (key.is_undefined()) key.set(Filter::context()->inbound());
  return true;
}

auto Mux::on_new_cluster() -> MuxBase::SessionCluster* {
  if (auto f = m_options_f.get()) {
    pjs::Value opts;
    if (!Filter::eval(f, opts)) return nullptr;
    if (!opts.is_object()) {
      Filter::error("callback did not return an object for options");
      return nullptr;
    }
    try {
      Options options(opts.o());
      return new SessionCluster(this, options);
    } catch (std::runtime_error &err) {
      Filter::error(err.what());
      return nullptr;
    }
  } else {
    return new SessionCluster(this, m_options);
  }
}

auto Mux::on_new_pipeline(EventTarget::Input *output, pjs::Value args[2]) -> Pipeline* {
  return Filter::sub_pipeline(0, true, output, nullptr, 2, args);
}

//
// Mux::Stream
//

void Mux::Stream::on_event(Event *evt) {
  if (auto start = evt->as<MessageStart>()) {
    if (!m_start) {
      m_start = start;
    }

  } else if (auto data = evt->as<Data>()) {
    if (m_start) {
      m_buffer.push(*data);
    }

  } else if (evt->is<MessageEnd>() || evt->is<StreamEnd>()) {
    if (m_start) {
      auto inp = m_output.get();
      inp->input(m_start);
      if (!m_buffer.empty()) {
        inp->input(Data::make(std::move(m_buffer)));
      }
      inp->input(evt->is<StreamEnd>() ? MessageEnd::make() : evt);
    }
  }
}

} // namespace pipy
