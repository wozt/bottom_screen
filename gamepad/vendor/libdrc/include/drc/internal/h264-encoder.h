// Copyright (c) 2013, Mema Hacking, All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#pragma once

#include <array>
#include <cstdio>
#include <drc/types.h>
#include <tuple>
#include <vector>

extern "C" {
#include <x264.h>
}

namespace drc {

// A chunk is defined as a pointer to encoded data and the size of that data.
// There are 5 chunks in a frame.
const int kH264ChunksPerFrame = 5;
typedef std::tuple<const byte*, size_t> H264Chunk;
typedef std::array<H264Chunk, kH264ChunksPerFrame> H264ChunkArray;

class H264Encoder {
 public:
  H264Encoder();
  virtual ~H264Encoder();

  // The returned chunk array is only valid until the next call to Encode.
  const H264ChunkArray& Encode(const std::vector<byte>& frame, bool idr);

  // True if the frame returned by the last Encode() call was actually coded
  // as an IDR. x264 also emits IDRs on its own (keyint), not just when we ask
  // for one, and the GamePad only treats a chunk as a recovery point when the
  // vstrm IDR flag is set - so the flag must follow the real NAL type.
  bool CurrentFrameIsIdr() const { return curr_frame_idr_; }

  // Throw the encoder away and start a new one, so that the next Encode()
  // really does produce an IDR.
  //
  // With intra refresh enabled x264 answers a forced IDR by restarting its
  // refresh wave instead, and never emits a NAL_SLICE_IDR again after the
  // first frame. That repairs ordinary loss, but it leaves nothing that can
  // recover a decoder which has lost the sequence outright - the GamePad then
  // freezes for good while audio keeps playing. A fresh encoder is the only
  // way back.
  void Restart();

 private:
  void ProcessNalUnit(x264_nal_t* nal);
  static void ProcessNalUnitTrampoline(x264_t* h, x264_nal_t* nal, void* arg);

  void CreateEncoder();
  void DestroyEncoder();

  x264_t* encoder_;
  H264ChunkArray chunks_;
  int num_chunks_encoded_;
  bool curr_frame_idr_;
  FILE* dump_file_;
};

}  // namespace drc
