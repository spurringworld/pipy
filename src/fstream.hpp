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

#ifndef FSTREAM_HPP
#define FSTREAM_HPP

#include "net.hpp"
#include "event.hpp"
#include "input.hpp"
#include "os-platform.hpp"

#include <stdio.h>

namespace pipy {

class Data;

//
// FileStream
//

class FileStream :
  public pjs::RefCount<FileStream>,
  public pjs::Pooled<FileStream>,
  public EventFunction,
  public InputSource,
  public FlushTarget
{
public:
#ifdef _WIN32
  typedef asio::windows::random_access_handle stream_t;
#else
  typedef asio::posix::stream_descriptor stream_t;
#endif

  static auto make(size_t read_size, os::FileHandle fd, Data::Producer *dp) -> FileStream* {
    return new FileStream(read_size, fd, dp);
  }

  auto fd() const -> os::FileHandle { return m_fd; }
  void set_no_close() { m_no_close = true; }
  void set_buffer_limit(size_t size) { m_buffer_limit = size; }
  void close();

private:
  FileStream(size_t read_size, os::FileHandle fd, Data::Producer *dp);

  virtual void on_event(Event *evt) override;
  virtual void on_flush() override;
  virtual void on_tap_open() override;
  virtual void on_tap_close() override;

  enum ReceivingState {
    RECEIVING,
    PAUSING,
    PAUSED,
  };

  stream_t m_stream;
  os::FileHandle m_fd;
  Data::Producer* m_dp;
  Data m_buffer;
  size_t m_buffer_limit = 0;
  size_t m_file_pointer = 0;
  ReceivingState m_receiving_state = RECEIVING;
  int m_read_size;
  bool m_no_close = false;
  bool m_overflowed = false;
  bool m_pumping = false;
  bool m_ended = false;
  bool m_closed = false;

  void read();
  void write(Data *data);
  void end();
  void pump();

  friend class pjs::RefCount<FileStream>;
};

} // namespace pipy

#endif // FSTREAM_HPP
