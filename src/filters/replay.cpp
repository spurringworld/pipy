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

#include "replay.hpp"
#include "pipeline.hpp"

namespace pipy {


//
// Replay
//

Replay::Replay()
{
}

Replay::Replay(const Replay &r)
  : Filter(r)
{
}

Replay::~Replay()
{
}

void Replay::dump(Dump &d) {
  Filter::dump(d);
  d.name = "replay";
}

auto Replay::clone() -> Filter* {
  return new Replay(*this);
}

void Replay::reset() {
  Filter::reset();
  m_buffer.clear();
  m_pipeline = nullptr;
  m_timer.cancel();
  m_replay_scheduled = false;
}

void Replay::process(Event *evt) {
  if (!m_pipeline) {
    m_pipeline = sub_pipeline(0, false, ReplayReceiver::input());
  }
  m_buffer.push(evt);
  Filter::output(evt, m_pipeline->input());
}

void Replay::schedule_replay() {
  if (!m_replay_scheduled) {
    m_timer.schedule(0, [this]() {
      m_replay_scheduled = false;
      replay();
    });
    m_replay_scheduled = true;
  }
}

void Replay::replay() {
  InputContext ic;
  Pipeline::auto_release(m_pipeline);
  m_pipeline = sub_pipeline(0, false, ReplayReceiver::input());
  m_buffer.iterate(
    [this](Event *evt) {
      Filter::output(evt->clone(), m_pipeline->input());
    }
  );
}

//
// ReplayReceiver
//

void ReplayReceiver::on_event(Event *evt) {
  auto *filter = static_cast<Replay*>(this);
  if (auto *end = evt->as<StreamEnd>()) {
    if (end->error() == StreamEnd::Error::REPLAY) {
      filter->schedule_replay();
      return;
    }
  }
  filter->Filter::output(evt);
}

} // namespace pipy
