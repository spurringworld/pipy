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

#include "buffer.hpp"
#include "logging.hpp"

namespace pipy {

Buffer::Buffer(const pjs::Value &filename, const Options &options)
  : m_filename(filename)
  , m_options(options)
{
}

Buffer::Buffer(const Buffer &r)
  : Filter(r)
  , m_filename(r.m_filename)
  , m_options(r.m_options)
{
}

Buffer::~Buffer() {
}

void Buffer::dump(std::ostream &out) {
  out << "buffer";
}

auto Buffer::clone() -> Filter* {
  return new Buffer(*this);
}

void Buffer::reset() {
  Filter::reset();
  if (m_file_w) {
    m_file_w->close();
    m_file_w = nullptr;
  }
  if (m_file_r) {
    m_file_r->close();
    m_file_r = nullptr;
  }
  m_resolved_filename = nullptr;
}

void Buffer::process(Event *evt) {
  if (auto *data = evt->as<Data>()) {
    if (!data->empty()) {
      if (m_buffer.size() < m_options.threshold) {
        m_buffer.push(*data);
        output(evt);
      } else {
        if (!m_resolved_filename) {
          pjs::Value filename;
          if (!eval(m_filename, filename)) return;
          auto *s = filename.to_string();
          m_resolved_filename = s;
          s->release();
          m_file_w = File::make(m_resolved_filename->str());
          m_file_w->open_write();
          if (!m_buffer.empty()) {
            m_file_w->write(m_buffer);
          }
        }
        if (m_file_w) {
          m_file_w->write(*data);
        }
      }
    }

  } else if (evt->is<StreamEnd>()) {
    if (m_file_w) {
      m_file_w->close();
      m_file_w = nullptr;
    }
    if (!m_file_r) {
      m_file_r = File::make(m_resolved_filename->str());
      m_file_r->open_read(
        m_buffer.size(),
        [this](FileStream *fs) {
          if (fs) {
            fs->chain(output());
          }
        }
      );
    }
  }
}

} // namespace pipy
