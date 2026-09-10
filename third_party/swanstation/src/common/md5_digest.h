#pragma once
#include "types.h"

// based heavily on this implementation:
// http://www.fourmilab.ch/md5/

class MD5Digest
{
public:
  MD5Digest();

  void Update(const void* pData, uint32_t cbData);
  void Final(uint8_t Digest[16]);
  void Reset();

private:
  uint32_t buf[4];
  uint32_t bits[2];
  uint8_t in[64];
};
