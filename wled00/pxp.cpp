#include "wled.h"

#ifdef WLED_ENABLE_PXP

#include "pxp.h"

namespace {

constexpr uint16_t PXP_MAGIC_VERSION = 0x5010;
constexpr uint16_t PXP_FLAG_COMPRESSED = 0x0001;
constexpr uint16_t PXP_FLAG_RESERVED = 0x000E;
constexpr uint8_t PXP_VERSION = 1;

constexpr uint8_t PXP_CMD_DISCOVER_REQUEST = 0x01;
constexpr uint8_t PXP_CMD_DISCOVER_REPLY = 0x02;
constexpr uint8_t PXP_CMD_TARGET_RANGE = 0x10;
constexpr uint8_t PXP_CMD_TARGET_SPARSE = 0x11;
constexpr uint8_t PXP_CMD_TARGET_SPARSE_SEGMENTS = 0x12;
constexpr uint8_t PXP_CMD_PIXEL_DATA = 0x20;
constexpr uint8_t PXP_CMD_VERIFY = 0x30;
constexpr uint8_t PXP_CMD_VERIFY_REPORT = 0x31;
constexpr uint8_t PXP_CMD_ERROR = 0x32;
constexpr uint8_t PXP_CMD_TIME_SYNC = 0x40;
constexpr uint8_t PXP_CMD_SCHEDULE = 0x41;

constexpr uint8_t PXP_PIXEL_FORMAT_RGB888 = 0;
constexpr uint8_t PXP_PIXEL_RAW = 0;
constexpr uint8_t PXP_PIXEL_FILL = 1;
constexpr uint8_t PXP_PIXEL_PALETTE_INDEX = 2;
constexpr uint8_t PXP_PIXEL_COLOR_PLANES_DELTA_PACKED = 3;
constexpr uint8_t PXP_INDEX_ABSOLUTE_VARUINT = 0;
constexpr uint8_t PXP_INDEX_DELTA_VARUINT = 1;

constexpr uint8_t PXP_ERROR_GENERIC = 0;
constexpr uint8_t PXP_ERROR_UNKNOWN_COMMAND = 1;
constexpr uint8_t PXP_ERROR_BAD_ARGUMENTS = 2;
constexpr uint8_t PXP_ERROR_UNSUPPORTED = 3;
constexpr uint8_t PXP_ERROR_LENGTH_MISMATCH = 4;
constexpr uint8_t PXP_ERROR_COMPRESSION = 5;
constexpr uint8_t PXP_ERROR_BUFFER_FULL = 6;

constexpr uint16_t PXP_LZSS_WINDOW = 512;

#ifdef WLED_ENABLE_PXP_TIMING
#ifndef PXP_TIMING_ARENA_SIZE
  #ifdef ARDUINO_ARCH_ESP32
    #define PXP_TIMING_ARENA_SIZE 16384
  #else
    #define PXP_TIMING_ARENA_SIZE 4096
  #endif
#endif
#ifndef PXP_TIMING_QUEUE_ENTRIES
  #if PXP_TIMING_ARENA_SIZE < 400
    #define PXP_TIMING_QUEUE_ENTRIES 1
  #elif (PXP_TIMING_ARENA_SIZE / 400) > UINT8_MAX
    #define PXP_TIMING_QUEUE_ENTRIES UINT8_MAX
  #else
    #define PXP_TIMING_QUEUE_ENTRIES (PXP_TIMING_ARENA_SIZE / 400)
  #endif
#endif
#ifndef PXP_TIMING_LATE_GRACE_MS
  #define PXP_TIMING_LATE_GRACE_MS 25
#endif
#ifndef PXP_TIMING_RESYNC_THRESHOLD_MS
  #define PXP_TIMING_RESYNC_THRESHOLD_MS 5000
#endif
static_assert((PXP_TIMING_ARENA_SIZE % 4) == 0, "PXP_TIMING_ARENA_SIZE must be 4-byte aligned");
static_assert((PXP_TIMING_ARENA_SIZE / 4) <= UINT16_MAX, "PXP timing arena must fit 16-bit word offsets");
static_assert(PXP_TIMING_QUEUE_ENTRIES > 0 && PXP_TIMING_QUEUE_ENTRIES <= UINT8_MAX, "PXP timing queue entry count must fit uint8_t counters");

constexpr uint8_t PXP_ARENA_MAGIC = 0xC7;
constexpr uint8_t PXP_ARENA_USED = 0x01;
constexpr uint8_t PXP_LATE_APPLY = 0;
constexpr uint8_t PXP_LATE_DROP = 1;
constexpr uint8_t PXP_BUF_FULL_QUEUE = 1;
constexpr uint8_t PXP_BUF_FULL_FRAME = 2;
constexpr uint8_t PXP_BUF_FULL_ARENA = 3;
constexpr uint8_t PXP_BUF_FULL_INSERT = 4;

struct PxpArenaHeader {
  uint16_t lenWords;
  uint8_t flags;
  uint8_t magic;
};

struct PxpScheduledEntry {
  uint32_t dueTimeMs;
  uint32_t packetTag;
  uint16_t sequence;
  uint16_t offsetWords;
  uint8_t latePolicy;
  uint8_t commandType;
};

struct PxpPacketSchedule {
  bool active = false;
  uint32_t senderDueMs = 0;
  uint32_t localDueMs = 0;
  uint8_t latePolicy = PXP_LATE_APPLY;
};

alignas(4) uint8_t pxpScheduleArena[PXP_TIMING_ARENA_SIZE];
PxpScheduledEntry pxpScheduleEntries[PXP_TIMING_QUEUE_ENTRIES];
uint8_t pxpScheduleCount = 0;
uint16_t pxpScheduleSequence = 0;
bool pxpArenaReady = false;
bool pxpTimingSynced = false;
uint32_t pxpTimingSenderSyncMs = 0;
uint32_t pxpTimingLocalSyncMs = 0;
IPAddress pxpQueueResponseIp;
uint16_t pxpQueueResponsePort = 0;
bool pxpQueueResponseEndpointValid = false;
uint32_t pxpLastQueueTraceMs = 0;
uint32_t pxpLastLateDropTraceMs = 0;
uint32_t pxpLastTimingTraceMs = 0;
uint16_t pxpTimeSyncTraceCount = 0;
uint16_t pxpScheduleTraceCount = 0;

void pxpDebugLogBufferFull(uint8_t reason, uint32_t packetTag, uint8_t commandType, uint32_t commandLen, uint32_t frameLen, const PxpPacketSchedule& schedule)
{
  DEBUG_PRINTF_P(PSTR("PXP buffer full: reason=%u tag=%lu cmd=0x%02X cmdLen=%lu frameLen=%lu q=%u/%u arena=%uB dueLocal=%lu now=%lu late=%u\n"),
                 reason, (unsigned long)packetTag, commandType, (unsigned long)commandLen, (unsigned long)frameLen,
                 pxpScheduleCount, PXP_TIMING_QUEUE_ENTRIES, PXP_TIMING_ARENA_SIZE,
                 (unsigned long)schedule.localDueMs, (unsigned long)millis(), schedule.latePolicy);
}

void pxpDebugLogScheduleWithoutFrame(uint32_t packetTag, uint32_t commandOrdinal, const PxpPacketSchedule& schedule, uint8_t queuedCommands, uint8_t queuedPixels)
{
  DEBUG_PRINTF_P(PSTR("PXP schedule without frame: tag=%lu ord=%lu queued=%u pixels=%u q=%u/%u senderDue=%lu localDue=%lu now=%lu late=%u\n"),
                 (unsigned long)packetTag, (unsigned long)commandOrdinal, queuedCommands, queuedPixels,
                 pxpScheduleCount, PXP_TIMING_QUEUE_ENTRIES, (unsigned long)schedule.senderDueMs,
                 (unsigned long)schedule.localDueMs, (unsigned long)millis(), schedule.latePolicy);
}

bool pxpShouldTraceTiming()
{
  const uint32_t now = millis();
  if (int32_t(now - pxpLastTimingTraceMs) > 1000) {
    pxpLastTimingTraceMs = now;
    return true;
  }
  return false;
}

void pxpDebugLogTimeSync(uint32_t packetTag, uint32_t commandOrdinal, uint32_t senderTime, uint32_t receiveMs, bool wasSynced)
{
  if (pxpTimeSyncTraceCount < 4 || pxpShouldTraceTiming()) {
    if (pxpTimeSyncTraceCount < UINT16_MAX) pxpTimeSyncTraceCount++;
    DEBUG_PRINTF_P(PSTR("PXP TimeSync: tag=%lu ord=%lu sender=%lu receive=%lu wasSynced=%u syncSender=%lu syncLocal=%lu q=%u/%u\n"),
                   (unsigned long)packetTag, (unsigned long)commandOrdinal, (unsigned long)senderTime,
                   (unsigned long)receiveMs, wasSynced, (unsigned long)pxpTimingSenderSyncMs,
                   (unsigned long)pxpTimingLocalSyncMs, pxpScheduleCount, PXP_TIMING_QUEUE_ENTRIES);
  }
}

void pxpDebugLogScheduleSet(uint32_t packetTag, uint32_t commandOrdinal, const PxpPacketSchedule& schedule, bool relative)
{
  const uint32_t now = millis();
  if (pxpScheduleTraceCount < 8 || pxpShouldTraceTiming()) {
    if (pxpScheduleTraceCount < UINT16_MAX) pxpScheduleTraceCount++;
    DEBUG_PRINTF_P(PSTR("PXP Schedule: tag=%lu ord=%lu rel=%u senderDue=%lu localDue=%lu now=%lu in=%ld late=%u q=%u/%u synced=%u\n"),
                   (unsigned long)packetTag, (unsigned long)commandOrdinal, relative,
                   (unsigned long)schedule.senderDueMs, (unsigned long)schedule.localDueMs,
                   (unsigned long)now, (long)(schedule.localDueMs - now), schedule.latePolicy,
                   pxpScheduleCount, PXP_TIMING_QUEUE_ENTRIES, pxpTimingSynced);
  }
}
#endif

/*
 * PXP parser vectors used while implementing this receiver:
 * - DiscoverRequest: 10 50 01 00 00 00 02 00 01 00
 * - TargetRange 0..2 + Fill FF B0 60:
 *   10 50 02 00 00 00 0A 00 10 02 00 03 20 04 01 FF B0 60
 * - TargetRange 0..2 + Raw red/green/blue:
 *   10 50 03 00 00 00 10 00 10 02 00 03 20 0A 00 FF 00 00 00 FF 00 00 00 FF
 * - TargetSparse delta [91,92,340,566,702] + Fill white:
 *   10 50 04 00 00 00 12 00 11 0A 05 01 5B 01 F8 01 E2 01 88 01 20 04 01 FF FF FF
 * - TargetRange 0 + ColorPlanesDeltaPacked single RGB888 pixel [10 20 30]:
 *   10 50 07 00 00 00 0A 00 10 02 00 01 20 04 03 00 00 10 20 30
 * - ColorPlanesDeltaPacked RGB888 ramp uses delta_bits_packed [44 04] for R/G/B 4-bit deltas.
 * - LZSS DiscoverRequest literals: 11 50 06 00 00 00 03 00 00 01 00
 */

uint8_t pxpPacket[PXP_MAX_PACKET_SIZE];
uint8_t pxpTargetData[PXP_MAX_PACKET_SIZE];
uint8_t pxpVerifyScratch[PXP_MAX_PACKET_SIZE];
uint8_t pxpResponse[PXP_MAX_PACKET_SIZE];

enum class ReadStatus : uint8_t {
  Ok,
  End,
  CompressionError
};

class PxpReader {
public:
  PxpReader(const uint8_t* data, uint16_t len, bool compressed)
    : _data(data), _len(len), _compressed(compressed)
  {
    reset();
  }

  void reset()
  {
    _pos = 0;
    _flags = 0;
    _flagsRemaining = 0;
    _matchDistance = 0;
    _matchRemaining = 0;
    _ringWrite = 0;
    _decoded = 0;
    memset(_ring, 0, sizeof(_ring));
  }

  ReadStatus read(uint8_t& out)
  {
    if (!_compressed) {
      if (_pos >= _len) return ReadStatus::End;
      out = _data[_pos++];
      return ReadStatus::Ok;
    }

    for (;;) {
      if (_matchRemaining) {
        out = _ring[(_ringWrite + PXP_LZSS_WINDOW - _matchDistance) & (PXP_LZSS_WINDOW - 1)];
        push(out);
        _matchRemaining--;
        return ReadStatus::Ok;
      }

      if (_pos >= _len) return ReadStatus::End;

      if (!_flagsRemaining) {
        _flags = _data[_pos++];
        _flagsRemaining = 8;
        if (_pos >= _len) return ReadStatus::CompressionError;
      }

      const bool match = _flags & 0x01;
      _flags >>= 1;
      _flagsRemaining--;

      if (!match) {
        if (_pos >= _len) return ReadStatus::CompressionError;
        out = _data[_pos++];
        push(out);
        return ReadStatus::Ok;
      }

      if (_pos + 1 >= _len) return ReadStatus::CompressionError;
      const uint16_t token = uint16_t(_data[_pos]) | (uint16_t(_data[_pos + 1]) << 8);
      _pos += 2;
      _matchDistance = (token & 0x01FF) + 1;
      _matchRemaining = (token >> 9) + 3;
      if (_matchDistance > PXP_LZSS_WINDOW || _matchDistance > _decoded) return ReadStatus::CompressionError;
    }
  }

  bool compressed() const { return _compressed; }

private:
  void push(uint8_t value)
  {
    _ring[_ringWrite] = value;
    _ringWrite = (_ringWrite + 1) & (PXP_LZSS_WINDOW - 1);
    if (_decoded < UINT32_MAX) _decoded++;
  }

  const uint8_t* _data;
  uint16_t _len;
  uint16_t _pos = 0;
  bool _compressed;
  uint8_t _flags = 0;
  uint8_t _flagsRemaining = 0;
  uint16_t _matchDistance = 0;
  uint8_t _matchRemaining = 0;
  uint16_t _ringWrite = 0;
  uint32_t _decoded = 0;
  uint8_t _ring[PXP_LZSS_WINDOW];
};

struct PxpError {
  uint8_t code = PXP_ERROR_GENERIC;
  uint8_t command = 0;
};

struct PxpCommandReader {
  PxpReader& source;
  uint32_t remaining;
  bool compressionError = false;
  bool lengthError = false;

  PxpCommandReader(PxpReader& src, uint32_t len) : source(src), remaining(len) {}

  bool read(uint8_t& out)
  {
    if (!remaining) {
      lengthError = true;
      return false;
    }

    const ReadStatus status = source.read(out);
    if (status == ReadStatus::Ok) {
      remaining--;
      return true;
    }

    if (status == ReadStatus::CompressionError || source.compressed()) compressionError = true;
    else lengthError = true;
    return false;
  }

  bool skip()
  {
    uint8_t value;
    while (remaining) {
      if (!read(value)) return false;
    }
    return true;
  }
};

struct PxpWriteBuffer {
  uint8_t* data;
  uint16_t capacity;
  uint16_t pos = 0;

  PxpWriteBuffer(uint8_t* buffer, uint16_t maxLen) : data(buffer), capacity(maxLen) {}

  bool write(uint8_t value)
  {
    if (pos >= capacity) return false;
    data[pos++] = value;
    return true;
  }

  bool writeLe16(uint16_t value)
  {
    return write(value & 0xFF) && write(value >> 8);
  }

  bool writeLe32(uint32_t value)
  {
    return write(value & 0xFF) && write((value >> 8) & 0xFF) &&
           write((value >> 16) & 0xFF) && write((value >> 24) & 0xFF);
  }

  bool writeVarUInt(uint32_t value)
  {
    do {
      uint8_t byte = value & 0x7F;
      value >>= 7;
      if (value) byte |= 0x80;
      if (!write(byte)) return false;
    } while (value);
    return true;
  }
};

enum class TargetMode : uint8_t {
  None,
  Range,
  Sparse,
  SparseSegments
};

struct PxpTarget {
  TargetMode mode = TargetMode::None;
  uint32_t start = 0;
  uint32_t count = 0;
  uint32_t entries = 0;
  uint16_t payloadLen = 0;
};

#ifdef WLED_ENABLE_PXP_TIMING
uint8_t pxpScheduledTargetData[PXP_MAX_PACKET_SIZE];
PxpTarget pxpScheduledTarget;
#endif

bool readVarUInt(PxpReader& reader, uint32_t& value)
{
  value = 0;
  uint32_t shift = 0;
  uint8_t bytes = 0;

  for (;;) {
    uint8_t byte;
    const ReadStatus status = reader.read(byte);
    if (status != ReadStatus::Ok) return false;
    if (bytes == 4 && (byte & 0xF0)) return false;

    value |= uint32_t(byte & 0x7F) << shift;
    bytes++;

    if (!(byte & 0x80)) {
      if (bytes > 1 && byte == 0) return false;
      if (bytes > 1 && value < (uint32_t(1) << (7 * (bytes - 1)))) return false;
      return true;
    }

    if (bytes >= 5) return false;
    shift += 7;
  }
}

bool readVarUInt(PxpCommandReader& reader, uint32_t& value)
{
  value = 0;
  uint32_t shift = 0;
  uint8_t bytes = 0;

  for (;;) {
    uint8_t byte;
    if (!reader.read(byte)) return false;
    if (bytes == 4 && (byte & 0xF0)) return false;

    value |= uint32_t(byte & 0x7F) << shift;
    bytes++;

    if (!(byte & 0x80)) {
      if (bytes > 1 && byte == 0) return false;
      if (bytes > 1 && value < (uint32_t(1) << (7 * (bytes - 1)))) return false;
      return true;
    }

    if (bytes >= 5) return false;
    shift += 7;
  }
}

bool readLe16(PxpCommandReader& reader, uint16_t& value)
{
  uint8_t lo, hi;
  if (!reader.read(lo) || !reader.read(hi)) return false;
  value = uint16_t(lo) | (uint16_t(hi) << 8);
  return true;
}

bool readLe32(PxpCommandReader& reader, uint32_t& value)
{
  uint8_t b0, b1, b2, b3;
  if (!reader.read(b0) || !reader.read(b1) || !reader.read(b2) || !reader.read(b3)) return false;
  value = uint32_t(b0) | (uint32_t(b1) << 8) | (uint32_t(b2) << 16) | (uint32_t(b3) << 24);
  return true;
}

bool validateRange(uint32_t start, uint32_t count)
{
  const uint32_t total = strip.getLengthTotal();
  return count > 0 && start < total && count <= total - start;
}

class TargetIterator {
public:
  TargetIterator(const PxpTarget& target, const uint8_t* targetData)
    : _target(target), _reader(targetData, target.payloadLen, false), _payloadReader{_reader, target.payloadLen}
  {
    if (_target.mode == TargetMode::Sparse || _target.mode == TargetMode::SparseSegments) {
      readVarUInt(_payloadReader, _entryCount);
      if (_target.mode == TargetMode::SparseSegments) _payloadReader.read(_segmentSizeMinusOne);
      _payloadReader.read(_indexEncoding);
    }
  }

  bool next(uint32_t& index)
  {
    switch (_target.mode) {
      case TargetMode::Range:
        if (_emitted >= _target.count) return false;
        index = _target.start + _emitted++;
        return true;
      case TargetMode::Sparse:
        return nextSparse(index);
      case TargetMode::SparseSegments:
        return nextSparseSegment(index);
      default:
        return false;
    }
  }

private:
  bool nextSparse(uint32_t& index)
  {
    if (_emitted >= _target.count) return false;
    uint32_t value;
    if (!readVarUInt(_payloadReader, value)) return false;
    if (_indexEncoding == PXP_INDEX_DELTA_VARUINT && _emitted) value += _lastIndex;
    _lastIndex = value;
    index = value;
    _emitted++;
    return true;
  }

  bool nextSparseSegment(uint32_t& index)
  {
    const uint32_t segmentSize = uint32_t(_segmentSizeMinusOne) + 1;
    if (_emitted >= _target.count) return false;

    if (!_segmentOffset) {
      uint32_t value;
      if (!readVarUInt(_payloadReader, value)) return false;
      if (_indexEncoding == PXP_INDEX_DELTA_VARUINT && _segmentIndex) value += _lastIndex;
      _lastIndex = value;
      _segmentStart = value;
      _segmentIndex++;
    }

    index = _segmentStart + _segmentOffset;
    _segmentOffset++;
    _emitted++;
    if (_segmentOffset >= segmentSize) _segmentOffset = 0;
    return true;
  }

  const PxpTarget& _target;
  PxpReader _reader;
  PxpCommandReader _payloadReader;
  uint32_t _entryCount = 0;
  uint32_t _emitted = 0;
  uint32_t _lastIndex = 0;
  uint32_t _segmentStart = 0;
  uint32_t _segmentIndex = 0;
  uint8_t _segmentSizeMinusOne = 0;
  uint8_t _segmentOffset = 0;
  uint8_t _indexEncoding = 0;
};

bool validateTargetPayload(TargetMode mode, const uint8_t* data, uint32_t len, PxpTarget& target, uint8_t& errorCode, uint8_t* targetData)
{
  if (len > PXP_MAX_PACKET_SIZE) {
    errorCode = PXP_ERROR_BUFFER_FULL;
    return false;
  }

  PxpReader payload(data, len, false);
  PxpCommandReader command{payload, len};

  if (mode == TargetMode::Range) {
    uint32_t start, count;
    if (!readVarUInt(command, start) || !readVarUInt(command, count) || command.remaining) {
      errorCode = PXP_ERROR_LENGTH_MISMATCH;
      return false;
    }
    if (!validateRange(start, count)) {
      errorCode = PXP_ERROR_BAD_ARGUMENTS;
      return false;
    }
    memcpy(targetData, data, len);
    target.mode = TargetMode::Range;
    target.start = start;
    target.count = count;
    target.entries = 1;
    target.payloadLen = len;
    return true;
  }

  uint32_t entries;
  if (!readVarUInt(command, entries) || entries == 0) {
    errorCode = PXP_ERROR_BAD_ARGUMENTS;
    return false;
  }

  uint8_t segmentSizeMinusOne = 0;
  if (mode == TargetMode::SparseSegments && !command.read(segmentSizeMinusOne)) {
    errorCode = command.compressionError ? PXP_ERROR_COMPRESSION : PXP_ERROR_LENGTH_MISMATCH;
    return false;
  }

  uint8_t indexEncoding;
  if (!command.read(indexEncoding) ||
      (indexEncoding != PXP_INDEX_ABSOLUTE_VARUINT && indexEncoding != PXP_INDEX_DELTA_VARUINT)) {
    errorCode = PXP_ERROR_BAD_ARGUMENTS;
    return false;
  }

  const uint32_t total = strip.getLengthTotal();
  const uint32_t segmentSize = uint32_t(segmentSizeMinusOne) + 1;
  uint32_t last = 0;
  for (uint32_t i = 0; i < entries; i++) {
    uint32_t value;
    if (!readVarUInt(command, value)) {
      errorCode = command.compressionError ? PXP_ERROR_COMPRESSION : PXP_ERROR_LENGTH_MISMATCH;
      return false;
    }
    if (indexEncoding == PXP_INDEX_DELTA_VARUINT && i > 0) {
      if (!value || value > UINT32_MAX - last) {
        errorCode = PXP_ERROR_BAD_ARGUMENTS;
        return false;
      }
      value += last;
    }
    if ((i > 0 && value <= last) ||
        (mode == TargetMode::SparseSegments && i > 0 && value < last + segmentSize) ||
        value >= total ||
        (mode == TargetMode::SparseSegments && segmentSize > total - value)) {
      errorCode = PXP_ERROR_BAD_ARGUMENTS;
      return false;
    }
    last = value;
  }

  if (command.remaining) {
    errorCode = PXP_ERROR_LENGTH_MISMATCH;
    return false;
  }

  if (mode == TargetMode::SparseSegments && entries > UINT32_MAX / segmentSize) {
    errorCode = PXP_ERROR_BAD_ARGUMENTS;
    return false;
  }
  const uint32_t targetCount = mode == TargetMode::Sparse ? entries : entries * segmentSize;
  if (!targetCount) {
    errorCode = PXP_ERROR_BAD_ARGUMENTS;
    return false;
  }
  memcpy(targetData, data, len);
  target.mode = mode;
  target.start = 0;
  target.entries = entries;
  target.count = targetCount;
  target.payloadLen = len;
  return true;
}

bool copyCommandPayload(PxpCommandReader& command, uint8_t* dest, uint32_t len)
{
  for (uint32_t i = 0; i < len; i++) {
    if (!command.read(dest[i])) return false;
  }
  return true;
}

void writePxpPixel(uint32_t index, uint8_t r, uint8_t g, uint8_t b)
{
  if (index > UINT16_MAX) return;
  setRealtimePixel(index, r, g, b, 0);
}

uint32_t readPxpPixel(uint32_t index)
{
  if (index > UINT16_MAX) return 0;
  return strip.getPixelColor(index + arlsOffset);
}

bool applyFill(PxpCommandReader& command, const PxpTarget& target, const uint8_t* targetData)
{
  uint8_t r, g, b;
  if (!command.read(r) || !command.read(g) || !command.read(b) || command.remaining) return false;

  TargetIterator iterator(target, targetData);
  uint32_t index;
  while (iterator.next(index)) writePxpPixel(index, r, g, b);
  return true;
}

bool applyRaw(PxpCommandReader& command, const PxpTarget& target, const uint8_t* targetData)
{
  TargetIterator iterator(target, targetData);
  uint32_t index;
  while (iterator.next(index)) {
    uint8_t r, g, b;
    if (!command.read(r) || !command.read(g) || !command.read(b)) return false;
    writePxpPixel(index, r, g, b);
  }
  return command.remaining == 0;
}

uint8_t bitsPerPaletteIndex(uint8_t count)
{
  if (count <= 2) return 1;
  if (count <= 4) return 2;
  if (count <= 8) return 3;
  return 4;
}

bool applyPalette(PxpCommandReader& command, const PxpTarget& target, const uint8_t* targetData)
{
  uint8_t header;
  if (!command.read(header) || (header & 0xF0)) return false;

  const uint8_t paletteCount = (header & 0x0F) + 1;
  uint8_t palette[16][3];
  for (uint8_t i = 0; i < paletteCount; i++) {
    if (!command.read(palette[i][0]) || !command.read(palette[i][1]) || !command.read(palette[i][2])) return false;
  }

  const uint8_t bits = bitsPerPaletteIndex(paletteCount);
  uint8_t bitOffset = 8;
  uint8_t packed = 0;

  TargetIterator iterator(target, targetData);
  uint32_t index;
  while (iterator.next(index)) {
    uint8_t colorIndex = 0;
    for (uint8_t bit = 0; bit < bits; bit++) {
      if (bitOffset >= 8) {
        if (!command.read(packed)) return false;
        bitOffset = 0;
      }
      if (packed & (1 << bitOffset)) colorIndex |= (1 << bit);
      bitOffset++;
    }
    if (colorIndex >= paletteCount) return false;
    writePxpPixel(index, palette[colorIndex][0], palette[colorIndex][1], palette[colorIndex][2]);
  }

  if (bitOffset < 8) {
    const uint8_t paddingMask = uint8_t(0xFF << bitOffset);
    if (packed & paddingMask) return false;
  }
  return command.remaining == 0;
}

class PxpBitReader {
public:
  PxpBitReader(PxpCommandReader& command) : _command(command) {}

  bool readBits(uint8_t bits, uint16_t& value)
  {
    value = 0;
    for (uint8_t bit = 0; bit < bits; bit++) {
      if (_bitOffset >= 8) {
        if (!_command.read(_current)) return false;
        _bitOffset = 0;
      }
      if (_current & (1 << _bitOffset)) value |= (1 << bit);
      _bitOffset++;
    }
    return true;
  }

private:
  PxpCommandReader& _command;
  uint8_t _current = 0;
  uint8_t _bitOffset = 8;
};

int16_t decodeZigZag(uint16_t value)
{
  return (value & 0x01) ? -int16_t((value >> 1) + 1) : int16_t(value >> 1);
}

bool applyColorPlanesDeltaPacked(PxpCommandReader& command, const PxpTarget& target, const uint8_t* targetData)
{
  uint8_t packed0, packed1;
  uint8_t current[3];
  if (!command.read(packed0) || !command.read(packed1) ||
      !command.read(current[0]) || !command.read(current[1]) || !command.read(current[2])) return false;

  if (packed1 & 0xF0) return false;
  const uint8_t widths[3] = { uint8_t(packed0 & 0x0F), uint8_t(packed0 >> 4), uint8_t(packed1 & 0x0F) };
  for (uint8_t i = 0; i < 3; i++) {
    if (widths[i] > 9) return false;
  }

  TargetIterator iterator(target, targetData);
  uint32_t index;
  if (!iterator.next(index)) return false;
  writePxpPixel(index, current[0], current[1], current[2]);

  PxpBitReader bits(command);
  while (iterator.next(index)) {
    for (uint8_t component = 0; component < 3; component++) {
      uint16_t encoded = 0;
      if (widths[component] && !bits.readBits(widths[component], encoded)) return false;
      const int16_t next = int16_t(current[component]) + decodeZigZag(encoded);
      if (next < 0 || next > 255) return false;
      current[component] = uint8_t(next);
    }
    writePxpPixel(index, current[0], current[1], current[2]);
  }

  return command.remaining == 0;
}

uint16_t crc16CcittUpdate(uint16_t crc, uint8_t value)
{
  crc ^= uint16_t(value) << 8;
  for (uint8_t i = 0; i < 8; i++) {
    crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  }
  return crc;
}

uint16_t crc16PxpSegment(uint32_t start, uint32_t count)
{
  uint16_t crc = 0xFFFF;
  for (uint32_t i = 0; i < count; i++) {
    const uint32_t color = readPxpPixel(start + i);
    crc = crc16CcittUpdate(crc, R(color));
    crc = crc16CcittUpdate(crc, G(color));
    crc = crc16CcittUpdate(crc, B(color));
  }
  return crc;
}

void writePacketHeader(PxpWriteBuffer& out, uint32_t packetTag, uint16_t payloadLen)
{
  out.writeLe16(PXP_MAGIC_VERSION);
  out.writeLe32(packetTag);
  out.writeLe16(payloadLen);
}

bool sendResponseTo(uint32_t packetTag, uint16_t payloadLen, const IPAddress& ip, uint16_t port)
{
  if (payloadLen > PXP_MAX_PACKET_SIZE - 8) return false;
  memmove(pxpResponse + 8, pxpResponse, payloadLen);
  PxpWriteBuffer header{pxpResponse, 8};
  writePacketHeader(header, packetTag, payloadLen);

  pxpUdp.beginPacket(ip, port);
  pxpUdp.write(pxpResponse, payloadLen + 8);
  return pxpUdp.endPacket() == 1;
}

bool sendResponse(uint32_t packetTag, uint16_t payloadLen)
{
  return sendResponseTo(packetTag, payloadLen, pxpUdp.remoteIP(), pxpUdp.remotePort());
}

bool sendErrorTo(uint32_t packetTag, uint32_t commandOrdinal, uint8_t commandType, uint8_t errorCode, const IPAddress& ip, uint16_t port)
{
  DEBUG_PRINTF_P(PSTR("PXP error: tag=%lu ord=%lu cmd=0x%02X err=%u peer=%u.%u.%u.%u:%u q=%u\n"),
                 (unsigned long)packetTag, (unsigned long)commandOrdinal, commandType, errorCode,
                 ip[0], ip[1], ip[2], ip[3], port,
#ifdef WLED_ENABLE_PXP_TIMING
                 pxpScheduleCount
#else
                 0
#endif
  );

  PxpWriteBuffer out{pxpResponse, PXP_MAX_PACKET_SIZE - 8};
  out.write(PXP_CMD_ERROR);

  PxpWriteBuffer payload{pxpResponse + 2, PXP_MAX_PACKET_SIZE - 10};
  payload.writeLe32(packetTag);
  payload.writeVarUInt(commandOrdinal);
  payload.write(commandType);
  payload.write(errorCode);

  pxpResponse[1] = payload.pos;
  return sendResponseTo(packetTag, payload.pos + 2, ip, port);
}

void sendError(uint32_t packetTag, uint32_t commandOrdinal, uint8_t commandType, uint8_t errorCode)
{
  sendErrorTo(packetTag, commandOrdinal, commandType, errorCode, pxpUdp.remoteIP(), pxpUdp.remotePort());
}

void fillDeviceId(uint8_t* out)
{
  memset(out, 0, 20);
  const char* name = (strcmp(serverDescription, "WLED") == 0 && strlen(cmDNS) > 0) ? cmDNS : serverDescription;
  for (uint8_t i = 0; i < 20 && name[i]; i++) {
    const char c = name[i];
    out[i] = (c >= 32 && c <= 126) ? c : '-';
  }
}

void sendDiscoverReply(uint32_t packetTag)
{
  PxpWriteBuffer out{pxpResponse, PXP_MAX_PACKET_SIZE - 8};
  out.write(PXP_CMD_DISCOVER_REPLY);

  PxpWriteBuffer payload{pxpResponse + 2, PXP_MAX_PACKET_SIZE - 10};
  fillDeviceId(payload.data);
  payload.pos = 20;
  payload.write(PXP_VERSION);
  payload.writeVarUInt(strip.getLengthTotal());
  payload.writeLe16(PXP_MAX_PACKET_SIZE);
  payload.write(PXP_PIXEL_FORMAT_RGB888);
  payload.writeLe16(0x000F); // Raw, Fill, PaletteIndex, ColorPlanesDeltaPacked
  payload.write(0x07);       // Range, Sparse, SparseSegments
  payload.write(0x03);       // AbsoluteVarUInt, DeltaVarUInt
  uint16_t featureFlags = 0x0001; // Compression
#ifdef WLED_ENABLE_PXP_TIMING
  featureFlags |= 0x0002;        // Timing
#endif
  payload.writeLe16(featureFlags);
  payload.write(9);          // 512-byte LZSS window

  pxpResponse[1] = payload.pos;
  sendResponse(packetTag, payload.pos + 2);
}

bool sendVerifyReportTo(uint32_t packetTag, uint32_t verifyTag, uint32_t start, uint32_t count, uint8_t segmentSizeMinusOne, const uint8_t* dirty, uint16_t dirtyLen, const IPAddress& ip, uint16_t port)
{
  PxpWriteBuffer out{pxpResponse, PXP_MAX_PACKET_SIZE - 8};
  out.write(PXP_CMD_VERIFY_REPORT);

  PxpWriteBuffer payload{pxpResponse + 2, PXP_MAX_PACKET_SIZE - 10};
  payload.writeLe32(verifyTag);
  payload.writeVarUInt(start);
  payload.writeVarUInt(count);
  payload.write(segmentSizeMinusOne);
  for (uint16_t i = 0; i < dirtyLen; i++) {
    if (!payload.write(dirty[i])) return false;
  }

  if (payload.pos > 127) return false;
  pxpResponse[1] = payload.pos;
  return sendResponseTo(packetTag, payload.pos + 2, ip, port);
}

bool sendVerifyReport(uint32_t packetTag, uint32_t verifyTag, uint32_t start, uint32_t count, uint8_t segmentSizeMinusOne, const uint8_t* dirty, uint16_t dirtyLen)
{
  return sendVerifyReportTo(packetTag, verifyTag, start, count, segmentSizeMinusOne, dirty, dirtyLen, pxpUdp.remoteIP(), pxpUdp.remotePort());
}

bool handleVerifyTo(PxpCommandReader& command, uint32_t packetTag, PxpError& error, const IPAddress& ip, uint16_t port)
{
  uint32_t verifyTag, start, count;
  uint8_t segmentSizeMinusOne;
  if (!readLe32(command, verifyTag) || !readVarUInt(command, start) || !readVarUInt(command, count) ||
      !command.read(segmentSizeMinusOne)) {
    error.code = command.compressionError ? PXP_ERROR_COMPRESSION : PXP_ERROR_LENGTH_MISMATCH;
    return false;
  }

  if (!validateRange(start, count)) {
    error.code = PXP_ERROR_BAD_ARGUMENTS;
    return false;
  }

  const uint32_t segmentSize = uint32_t(segmentSizeMinusOne) + 1;
  const uint32_t segmentCount = (count + segmentSize - 1) / segmentSize;
  const uint16_t bitmapBytes = (segmentCount + 7) / 8;
  if (bitmapBytes > PXP_MAX_PACKET_SIZE - 32) {
    DEBUG_PRINTF_P(PSTR("PXP verify buffer full: pkt=%lu start=%lu count=%lu segSize=%lu segs=%lu bitmap=%u max=%u peer=%u.%u.%u.%u:%u\n"),
                   (unsigned long)packetTag, (unsigned long)start, (unsigned long)count,
                   (unsigned long)segmentSize, (unsigned long)segmentCount, bitmapBytes,
                   PXP_MAX_PACKET_SIZE - 32, ip[0], ip[1], ip[2], ip[3], port);
    error.code = PXP_ERROR_BUFFER_FULL;
    return false;
  }

  memset(pxpVerifyScratch, 0, bitmapBytes);
  bool mismatch = false;
  uint32_t firstMismatchSegment = UINT32_MAX;
  uint16_t firstMismatchExpected = 0;
  uint16_t firstMismatchActual = 0;
  for (uint32_t segment = 0; segment < segmentCount; segment++) {
    uint16_t expected;
    if (!readLe16(command, expected)) {
      error.code = command.compressionError ? PXP_ERROR_COMPRESSION : PXP_ERROR_LENGTH_MISMATCH;
      return false;
    }

    const uint32_t segmentStart = start + segment * segmentSize;
    const uint32_t remaining = count - segment * segmentSize;
    const uint32_t segmentCountPixels = segmentSize < remaining ? segmentSize : remaining;
    const uint16_t actual = crc16PxpSegment(segmentStart, segmentCountPixels);
    if (actual != expected) {
      pxpVerifyScratch[segment >> 3] |= 1 << (segment & 0x07);
      if (!mismatch) {
        firstMismatchSegment = segment;
        firstMismatchExpected = expected;
        firstMismatchActual = actual;
      }
      mismatch = true;
    }
  }

  if (command.remaining) {
    error.code = PXP_ERROR_LENGTH_MISMATCH;
    return false;
  }

  if (mismatch) {
    DEBUG_PRINTF_P(PSTR("PXP verify mismatch: pkt=%lu verify=%lu start=%lu count=%lu segSize=%lu segs=%lu firstSeg=%lu firstPix=%lu expected=0x%04X actual=0x%04X dirtyBytes=%u peer=%u.%u.%u.%u:%u\n"),
                   (unsigned long)packetTag, (unsigned long)verifyTag, (unsigned long)start, (unsigned long)count,
                   (unsigned long)segmentSize, (unsigned long)segmentCount, (unsigned long)firstMismatchSegment,
                   (unsigned long)(start + firstMismatchSegment * segmentSize), firstMismatchExpected, firstMismatchActual,
                   bitmapBytes, ip[0], ip[1], ip[2], ip[3], port);
  }

  if (mismatch && !sendVerifyReportTo(packetTag, verifyTag, start, count, segmentSizeMinusOne, pxpVerifyScratch, bitmapBytes, ip, port)) {
    DEBUG_PRINTF_P(PSTR("PXP verify report failed: pkt=%lu verify=%lu start=%lu count=%lu dirtyBytes=%u peer=%u.%u.%u.%u:%u\n"),
                   (unsigned long)packetTag, (unsigned long)verifyTag, (unsigned long)start,
                   (unsigned long)count, bitmapBytes, ip[0], ip[1], ip[2], ip[3], port);
    error.code = PXP_ERROR_BUFFER_FULL;
    return false;
  }
  return true;
}

bool handleVerify(PxpCommandReader& command, uint32_t packetTag, PxpError& error)
{
  return handleVerifyTo(command, packetTag, error, pxpUdp.remoteIP(), pxpUdp.remotePort());
}

bool handlePixelData(PxpCommandReader& command, const PxpTarget& target, const uint8_t* targetData, PxpError& error)
{
  if (target.mode == TargetMode::None) {
    error.code = PXP_ERROR_BAD_ARGUMENTS;
    return false;
  }

  uint8_t encoding;
  if (!command.read(encoding)) {
    error.code = command.compressionError ? PXP_ERROR_COMPRESSION : PXP_ERROR_LENGTH_MISMATCH;
    return false;
  }

  bool ok = false;
  switch (encoding) {
    case PXP_PIXEL_RAW:
      ok = applyRaw(command, target, targetData);
      break;
    case PXP_PIXEL_FILL:
      ok = applyFill(command, target, targetData);
      break;
    case PXP_PIXEL_PALETTE_INDEX:
      ok = applyPalette(command, target, targetData);
      break;
    case PXP_PIXEL_COLOR_PLANES_DELTA_PACKED:
      ok = applyColorPlanesDeltaPacked(command, target, targetData);
      break;
    default:
      error.code = PXP_ERROR_UNSUPPORTED;
      return false;
  }

  if (!ok) {
    error.code = command.compressionError ? PXP_ERROR_COMPRESSION : PXP_ERROR_LENGTH_MISMATCH;
    return false;
  }
  pxpNewData = true;
  return true;
}

#ifdef WLED_ENABLE_PXP_TIMING
bool pxpTimeDue(uint32_t due, uint32_t now)
{
  return int32_t(now - due) >= 0;
}

bool pxpTimeBefore(uint32_t a, uint32_t b)
{
  return int32_t(a - b) < 0;
}

bool pxpIsLate(uint32_t due, uint32_t now)
{
  return int32_t(now - due) > int32_t(PXP_TIMING_LATE_GRACE_MS);
}

int32_t pxpSenderTimeDiff(uint32_t newer, uint32_t older)
{
  uint32_t diff = (newer - older) & 0x7FFFFFFF;
  if (diff & 0x40000000) return int32_t(diff) - int32_t(0x80000000UL);
  return int32_t(diff);
}

uint32_t pxpMapSenderToLocal(uint32_t senderMs)
{
  return pxpTimingLocalSyncMs + pxpSenderTimeDiff(senderMs & 0x7FFFFFFF, pxpTimingSenderSyncMs);
}

void pxpApplyTimeSync(uint32_t senderMs, uint32_t receiveMs)
{
  senderMs &= 0x7FFFFFFF;
  if (!pxpTimingSynced) {
    pxpTimingSenderSyncMs = senderMs;
    pxpTimingLocalSyncMs = receiveMs;
    pxpTimingSynced = true;
    return;
  }

  const uint32_t mappedMs = pxpMapSenderToLocal(senderMs);
  const int32_t error = int32_t(receiveMs - mappedMs);
  if (error > int32_t(PXP_TIMING_RESYNC_THRESHOLD_MS) || error < -int32_t(PXP_TIMING_RESYNC_THRESHOLD_MS)) {
    DEBUG_PRINTF_P(PSTR("PXP timing resync: sender=%lu receive=%lu mapped=%lu error=%ld oldSender=%lu oldLocal=%lu q=%u/%u\n"),
                   (unsigned long)senderMs, (unsigned long)receiveMs, (unsigned long)mappedMs, (long)error,
                   (unsigned long)pxpTimingSenderSyncMs, (unsigned long)pxpTimingLocalSyncMs,
                   pxpScheduleCount, PXP_TIMING_QUEUE_ENTRIES);
    pxpTimingSenderSyncMs = senderMs;
    pxpTimingLocalSyncMs = receiveMs;
    return;
  }

  int32_t adjust = error / 8;
  if (adjust > 10) adjust = 10;
  else if (adjust < -10) adjust = -10;
  pxpTimingLocalSyncMs += adjust;
}

void pxpRecoverStaleScheduleTimebase(uint32_t senderDueMs, uint32_t receiveMs, uint32_t localDueMs)
{
  const int32_t scheduleError = int32_t(receiveMs - localDueMs);
  if (scheduleError <= int32_t(PXP_TIMING_RESYNC_THRESHOLD_MS) && scheduleError >= -int32_t(PXP_TIMING_RESYNC_THRESHOLD_MS)) return;

  DEBUG_PRINTF_P(PSTR("PXP schedule timebase stale: senderDue=%lu receive=%lu mappedDue=%lu error=%ld oldSender=%lu oldLocal=%lu q=%u/%u\n"),
                 (unsigned long)senderDueMs, (unsigned long)receiveMs, (unsigned long)localDueMs, (long)scheduleError,
                 (unsigned long)pxpTimingSenderSyncMs, (unsigned long)pxpTimingLocalSyncMs,
                 pxpScheduleCount, PXP_TIMING_QUEUE_ENTRIES);
  pxpTimingSenderSyncMs = senderDueMs;
  pxpTimingLocalSyncMs = receiveMs;
}

uint8_t pxpVarUIntLen(uint32_t value)
{
  uint8_t len = 1;
  while (value >= 0x80) {
    value >>= 7;
    len++;
  }
  return len;
}

void pxpWriteVarUIntRaw(uint8_t*& out, uint32_t value)
{
  do {
    uint8_t byte = value & 0x7F;
    value >>= 7;
    if (value) byte |= 0x80;
    *out++ = byte;
  } while (value);
}

uint16_t pxpArenaWords(uint32_t bytes)
{
  return uint16_t((bytes + 3) / 4);
}

PxpArenaHeader* pxpArenaHeaderAt(uint16_t offsetWords)
{
  return reinterpret_cast<PxpArenaHeader*>(pxpScheduleArena + uint32_t(offsetWords) * 4);
}

void pxpArenaInit()
{
  PxpArenaHeader* header = pxpArenaHeaderAt(0);
  header->lenWords = PXP_TIMING_ARENA_SIZE / 4;
  header->flags = 0;
  header->magic = PXP_ARENA_MAGIC;
  pxpArenaReady = true;
}

void pxpArenaCoalesce()
{
  if (!pxpArenaReady) pxpArenaInit();
  const uint16_t arenaWords = PXP_TIMING_ARENA_SIZE / 4;
  uint16_t offset = 0;
  while (offset < arenaWords) {
    PxpArenaHeader* header = pxpArenaHeaderAt(offset);
    if (header->magic != PXP_ARENA_MAGIC || header->lenWords == 0 || offset + header->lenWords > arenaWords) {
      pxpArenaInit();
      return;
    }

    if (!(header->flags & PXP_ARENA_USED)) {
      while (offset + header->lenWords < arenaWords) {
        PxpArenaHeader* next = pxpArenaHeaderAt(offset + header->lenWords);
        if (next->magic != PXP_ARENA_MAGIC || next->lenWords == 0 || next->flags & PXP_ARENA_USED) break;
        header->lenWords += next->lenWords;
      }
    }
    offset += header->lenWords;
  }
}

bool pxpArenaAlloc(uint16_t payloadBytes, uint16_t& offsetWords)
{
  if (!pxpArenaReady) pxpArenaInit();
  const uint16_t neededWords = pxpArenaWords(sizeof(PxpArenaHeader) + payloadBytes);
  const uint16_t arenaWords = PXP_TIMING_ARENA_SIZE / 4;

  for (uint8_t pass = 0; pass < 2; pass++) {
    if (pass) pxpArenaCoalesce();
    uint16_t offset = 0;
    while (offset < arenaWords) {
      PxpArenaHeader* header = pxpArenaHeaderAt(offset);
      if (header->magic != PXP_ARENA_MAGIC || header->lenWords == 0 || offset + header->lenWords > arenaWords) {
        pxpArenaInit();
        break;
      }

      if (!(header->flags & PXP_ARENA_USED) && header->lenWords >= neededWords) {
        const uint16_t remainingWords = header->lenWords - neededWords;
        if (remainingWords > 1) {
          PxpArenaHeader* next = pxpArenaHeaderAt(offset + neededWords);
          next->lenWords = remainingWords;
          next->flags = 0;
          next->magic = PXP_ARENA_MAGIC;
          header->lenWords = neededWords;
        }
        header->flags = PXP_ARENA_USED;
        header->magic = PXP_ARENA_MAGIC;
        offsetWords = offset;
        return true;
      }
      offset += header->lenWords;
    }
  }
  return false;
}

void pxpArenaFree(uint16_t offsetWords)
{
  PxpArenaHeader* header = pxpArenaHeaderAt(offsetWords);
  if (header->magic == PXP_ARENA_MAGIC) header->flags = 0;
}

uint8_t* pxpArenaPayload(uint16_t offsetWords)
{
  return pxpScheduleArena + uint32_t(offsetWords) * 4 + sizeof(PxpArenaHeader);
}

uint16_t pxpArenaPayloadBytes(uint16_t offsetWords)
{
  PxpArenaHeader* header = pxpArenaHeaderAt(offsetWords);
  if (header->magic != PXP_ARENA_MAGIC || header->lenWords == 0) return 0;
  return uint16_t(header->lenWords * 4 - sizeof(PxpArenaHeader));
}

bool pxpScheduleEntryBefore(const PxpScheduledEntry& a, const PxpScheduledEntry& b)
{
  if (a.dueTimeMs != b.dueTimeMs) return pxpTimeBefore(a.dueTimeMs, b.dueTimeMs);
  return uint16_t(a.sequence - b.sequence) & 0x8000;
}

bool pxpInsertScheduledEntry(const PxpScheduledEntry& entry)
{
  if (pxpScheduleCount >= PXP_TIMING_QUEUE_ENTRIES) return false;
  uint8_t pos = pxpScheduleCount;
  while (pos > 0 && pxpScheduleEntryBefore(entry, pxpScheduleEntries[pos - 1])) {
    pxpScheduleEntries[pos] = pxpScheduleEntries[pos - 1];
    pos--;
  }
  pxpScheduleEntries[pos] = entry;
  pxpScheduleCount++;
  return true;
}

void pxpRemoveScheduledEntry(uint8_t pos)
{
  for (uint8_t i = pos + 1; i < pxpScheduleCount; i++) {
    pxpScheduleEntries[i - 1] = pxpScheduleEntries[i];
  }
  if (pxpScheduleCount) pxpScheduleCount--;
}

bool pxpQueueCommand(PxpCommandReader& command, uint8_t commandType, uint32_t commandLen, const PxpPacketSchedule& schedule, uint32_t packetTag, PxpError& error)
{
  const uint32_t now = millis();
  if (schedule.latePolicy == PXP_LATE_DROP && pxpIsLate(schedule.localDueMs, now)) {
    if (int32_t(now - pxpLastLateDropTraceMs) > 1000) {
      pxpLastLateDropTraceMs = now;
      DEBUG_PRINTF_P(PSTR("PXP late-drop at enqueue: tag=%lu cmd=0x%02X len=%lu due=%lu now=%lu lateBy=%ld q=%u/%u senderDue=%lu\n"),
                     (unsigned long)packetTag, commandType, (unsigned long)commandLen,
                     (unsigned long)schedule.localDueMs, (unsigned long)now, (long)(now - schedule.localDueMs),
                     pxpScheduleCount, PXP_TIMING_QUEUE_ENTRIES, (unsigned long)schedule.senderDueMs);
    }
    if (!command.skip()) {
      error.code = command.compressionError ? PXP_ERROR_COMPRESSION : PXP_ERROR_LENGTH_MISMATCH;
      return false;
    }
    return true;
  }

  if (pxpScheduleCount >= PXP_TIMING_QUEUE_ENTRIES) {
    pxpDebugLogBufferFull(PXP_BUF_FULL_QUEUE, packetTag, commandType, commandLen, 0, schedule);
    error.code = PXP_ERROR_BUFFER_FULL;
    return false;
  }

  const uint32_t frameLen = 1 + pxpVarUIntLen(commandLen) + commandLen;
  if (frameLen > UINT16_MAX) {
    pxpDebugLogBufferFull(PXP_BUF_FULL_FRAME, packetTag, commandType, commandLen, frameLen, schedule);
    error.code = PXP_ERROR_BUFFER_FULL;
    return false;
  }

  uint16_t offsetWords;
  if (!pxpArenaAlloc(frameLen, offsetWords)) {
    pxpDebugLogBufferFull(PXP_BUF_FULL_ARENA, packetTag, commandType, commandLen, frameLen, schedule);
    error.code = PXP_ERROR_BUFFER_FULL;
    return false;
  }

  uint8_t* out = pxpArenaPayload(offsetWords);
  *out++ = commandType;
  pxpWriteVarUIntRaw(out, commandLen);
  for (uint32_t i = 0; i < commandLen; i++) {
    if (!command.read(*out++)) {
      pxpArenaFree(offsetWords);
      error.code = command.compressionError ? PXP_ERROR_COMPRESSION : PXP_ERROR_LENGTH_MISMATCH;
      return false;
    }
  }

  PxpScheduledEntry entry;
  entry.dueTimeMs = schedule.localDueMs;
  entry.packetTag = packetTag;
  entry.sequence = pxpScheduleSequence++;
  entry.offsetWords = offsetWords;
  entry.latePolicy = schedule.latePolicy;
  entry.commandType = commandType;
  if (!pxpInsertScheduledEntry(entry)) {
    pxpDebugLogBufferFull(PXP_BUF_FULL_INSERT, packetTag, commandType, commandLen, frameLen, schedule);
    pxpArenaFree(offsetWords);
    error.code = PXP_ERROR_BUFFER_FULL;
    return false;
  }
  if (commandType == PXP_CMD_PIXEL_DATA || int32_t(now - pxpLastQueueTraceMs) > 1000) {
    pxpLastQueueTraceMs = now;
    DEBUG_PRINTF_P(PSTR("PXP queued: tag=%lu cmd=0x%02X len=%lu due=%lu now=%lu in=%ld q=%u/%u offset=%u\n"),
                   (unsigned long)packetTag, commandType, (unsigned long)commandLen,
                   (unsigned long)entry.dueTimeMs, (unsigned long)now, (long)(entry.dueTimeMs - now),
                   pxpScheduleCount, PXP_TIMING_QUEUE_ENTRIES, offsetWords);
  }
  return true;
}

bool pxpIsSchedulableCommand(uint8_t commandType)
{
  return commandType == PXP_CMD_TARGET_RANGE || commandType == PXP_CMD_TARGET_SPARSE ||
         commandType == PXP_CMD_TARGET_SPARSE_SEGMENTS || commandType == PXP_CMD_PIXEL_DATA ||
         commandType == PXP_CMD_VERIFY;
}

void pxpResetTiming()
{
  pxpArenaInit();
  pxpScheduleCount = 0;
  pxpScheduleSequence = 0;
  pxpTimingSynced = false;
  pxpQueueResponseEndpointValid = false;
  pxpScheduledTarget = PxpTarget();
  pxpLastQueueTraceMs = 0;
  pxpLastLateDropTraceMs = 0;
  pxpLastTimingTraceMs = 0;
  pxpTimeSyncTraceCount = 0;
  pxpScheduleTraceCount = 0;
}
#endif

bool processPacketCommands(const uint8_t* payload, uint16_t payloadLen, bool compressed, uint32_t packetTag, uint32_t receiveMs, PxpError& error, uint32_t& errorOrdinal)
{
#ifndef WLED_ENABLE_PXP_TIMING
  (void)receiveMs;
#endif
  PxpReader reader(payload, payloadLen, compressed);
  PxpTarget target;
#ifdef WLED_ENABLE_PXP_TIMING
  PxpPacketSchedule schedule;
  uint8_t scheduledCommandCount = 0;
  uint8_t scheduledPixelCount = 0;
#endif
  uint32_t ordinal = 0;

  for (;;) {
    uint8_t commandType;
    const ReadStatus status = reader.read(commandType);
    if (status == ReadStatus::End) {
#ifdef WLED_ENABLE_PXP_TIMING
      if (schedule.active && scheduledPixelCount == 0) {
        pxpDebugLogScheduleWithoutFrame(packetTag, ordinal, schedule, scheduledCommandCount, scheduledPixelCount);
      }
#endif
      return true;
    }
    if (status == ReadStatus::CompressionError) {
      error.code = PXP_ERROR_COMPRESSION;
      error.command = 0;
      errorOrdinal = ordinal;
      return false;
    }

    uint32_t commandLen;
    if (!readVarUInt(reader, commandLen)) {
      error.code = compressed ? PXP_ERROR_COMPRESSION : PXP_ERROR_LENGTH_MISMATCH;
      error.command = commandType;
      errorOrdinal = ordinal;
      return false;
    }

    PxpCommandReader command{reader, commandLen};
    error.command = commandType;
    errorOrdinal = ordinal;

#ifdef WLED_ENABLE_PXP_TIMING
    if (schedule.active && pxpIsSchedulableCommand(commandType)) {
      if (!pxpQueueCommand(command, commandType, commandLen, schedule, packetTag, error)) return false;
      if (scheduledCommandCount < UINT8_MAX) scheduledCommandCount++;
      if (commandType == PXP_CMD_PIXEL_DATA && scheduledPixelCount < UINT8_MAX) scheduledPixelCount++;
      ordinal++;
      continue;
    }
#endif

    switch (commandType) {
      case PXP_CMD_DISCOVER_REQUEST:
        if (commandLen != 0) {
          error.code = PXP_ERROR_LENGTH_MISMATCH;
          return false;
        }
        sendDiscoverReply(packetTag);
        break;

      case PXP_CMD_TARGET_RANGE:
      case PXP_CMD_TARGET_SPARSE:
      case PXP_CMD_TARGET_SPARSE_SEGMENTS: {
        if (commandLen > PXP_MAX_PACKET_SIZE) {
          DEBUG_PRINTF_P(PSTR("PXP target buffer full: tag=%lu ord=%lu cmd=0x%02X len=%lu max=%u peer=%u.%u.%u.%u:%u\n"),
                         (unsigned long)packetTag, (unsigned long)ordinal, commandType, (unsigned long)commandLen,
                         PXP_MAX_PACKET_SIZE, pxpUdp.remoteIP()[0], pxpUdp.remoteIP()[1], pxpUdp.remoteIP()[2],
                         pxpUdp.remoteIP()[3], pxpUdp.remotePort());
          error.code = PXP_ERROR_BUFFER_FULL;
          return false;
        }
        if (!copyCommandPayload(command, pxpResponse, commandLen)) {
          error.code = command.compressionError ? PXP_ERROR_COMPRESSION : PXP_ERROR_LENGTH_MISMATCH;
          return false;
        }
        uint8_t targetError = PXP_ERROR_BAD_ARGUMENTS;
        const TargetMode mode = commandType == PXP_CMD_TARGET_RANGE ? TargetMode::Range :
                                commandType == PXP_CMD_TARGET_SPARSE ? TargetMode::Sparse :
                                TargetMode::SparseSegments;
        if (!validateTargetPayload(mode, pxpResponse, commandLen, target, targetError, pxpTargetData)) {
          error.code = targetError;
          return false;
        }
        break;
      }

      case PXP_CMD_PIXEL_DATA:
        realtimeIP = pxpUdp.remoteIP();
        realtimeLock(realtimeTimeoutMs, REALTIME_MODE_PXP);
        if (!realtimeOverride && !handlePixelData(command, target, pxpTargetData, error)) {
          DEBUG_PRINTF_P(PSTR("PXP PixelData failed: tag=%lu ord=%lu err=%u len=%lu targetMode=%u targetCount=%lu override=%u peer=%u.%u.%u.%u:%u\n"),
                         (unsigned long)packetTag, (unsigned long)ordinal, error.code, (unsigned long)commandLen,
                         (unsigned)target.mode, (unsigned long)target.count, realtimeOverride,
                         pxpUdp.remoteIP()[0], pxpUdp.remoteIP()[1], pxpUdp.remoteIP()[2], pxpUdp.remoteIP()[3], pxpUdp.remotePort());
          return false;
        }
        if (realtimeOverride && !command.skip()) {
          error.code = command.compressionError ? PXP_ERROR_COMPRESSION : PXP_ERROR_LENGTH_MISMATCH;
          DEBUG_PRINTF_P(PSTR("PXP PixelData skip failed: tag=%lu ord=%lu err=%u len=%lu override=%u peer=%u.%u.%u.%u:%u\n"),
                         (unsigned long)packetTag, (unsigned long)ordinal, error.code, (unsigned long)commandLen,
                         realtimeOverride, pxpUdp.remoteIP()[0], pxpUdp.remoteIP()[1], pxpUdp.remoteIP()[2],
                         pxpUdp.remoteIP()[3], pxpUdp.remotePort());
          return false;
        }
        break;

      case PXP_CMD_VERIFY:
        if (!handleVerify(command, packetTag, error)) return false;
        break;

#ifdef WLED_ENABLE_PXP_TIMING
      case PXP_CMD_TIME_SYNC: {
        if (ordinal != 0) {
          error.code = PXP_ERROR_BAD_ARGUMENTS;
          return false;
        }
        uint32_t senderTime;
        if (!readVarUInt(command, senderTime) || command.remaining) {
          error.code = command.compressionError ? PXP_ERROR_COMPRESSION : PXP_ERROR_LENGTH_MISMATCH;
          return false;
        }
        if (senderTime & 0x80000000UL) {
          error.code = PXP_ERROR_BAD_ARGUMENTS;
          return false;
        }
        const bool wasSynced = pxpTimingSynced;
        pxpApplyTimeSync(senderTime, receiveMs);
        pxpDebugLogTimeSync(packetTag, ordinal, senderTime, receiveMs, wasSynced);
        break;
      }

      case PXP_CMD_SCHEDULE: {
        uint32_t timeCode;
        if (!readVarUInt(command, timeCode)) {
          error.code = command.compressionError ? PXP_ERROR_COMPRESSION : PXP_ERROR_LENGTH_MISMATCH;
          return false;
        }
        const uint32_t timeMs = timeCode >> 1;
        if (timeCode & 0x01) {
          if (!schedule.active || command.remaining) {
            error.code = PXP_ERROR_BAD_ARGUMENTS;
            return false;
          }
          if (scheduledPixelCount == 0) {
            pxpDebugLogScheduleWithoutFrame(packetTag, ordinal, schedule, scheduledCommandCount, scheduledPixelCount);
          }
          schedule.senderDueMs = (schedule.senderDueMs + timeMs) & 0x7FFFFFFF;
          schedule.localDueMs += timeMs;
          scheduledCommandCount = 0;
          scheduledPixelCount = 0;
          pxpDebugLogScheduleSet(packetTag, ordinal, schedule, true);
        } else {
          uint8_t latePolicy;
          if (!command.read(latePolicy) || command.remaining) {
            error.code = command.compressionError ? PXP_ERROR_COMPRESSION : PXP_ERROR_LENGTH_MISMATCH;
            return false;
          }
          if (latePolicy > PXP_LATE_DROP) {
            error.code = PXP_ERROR_BAD_ARGUMENTS;
            return false;
          }
          if (schedule.active && scheduledPixelCount == 0) {
            pxpDebugLogScheduleWithoutFrame(packetTag, ordinal, schedule, scheduledCommandCount, scheduledPixelCount);
          }
          if (!pxpTimingSynced) pxpApplyTimeSync(timeMs, receiveMs);
          schedule.active = true;
          schedule.senderDueMs = timeMs & 0x7FFFFFFF;
          schedule.localDueMs = pxpMapSenderToLocal(schedule.senderDueMs);
          pxpRecoverStaleScheduleTimebase(schedule.senderDueMs, receiveMs, schedule.localDueMs);
          schedule.localDueMs = pxpMapSenderToLocal(schedule.senderDueMs);
          schedule.latePolicy = latePolicy;
          scheduledCommandCount = 0;
          scheduledPixelCount = 0;
          pxpDebugLogScheduleSet(packetTag, ordinal, schedule, false);
        }
        break;
      }
#else
      case PXP_CMD_TIME_SYNC:
      case PXP_CMD_SCHEDULE:
        error.code = PXP_ERROR_UNSUPPORTED;
        return false;
#endif

      case PXP_CMD_DISCOVER_REPLY:
      case PXP_CMD_VERIFY_REPORT:
      case PXP_CMD_ERROR:
        error.code = PXP_ERROR_UNSUPPORTED;
        return false;

      default:
        error.code = PXP_ERROR_UNKNOWN_COMMAND;
        return false;
    }

    if (command.compressionError) {
      error.code = PXP_ERROR_COMPRESSION;
      return false;
    }
    if (command.remaining) {
      if (!command.skip()) {
        error.code = command.compressionError ? PXP_ERROR_COMPRESSION : PXP_ERROR_LENGTH_MISMATCH;
        return false;
      }
    }
    ordinal++;
  }
}

#ifdef WLED_ENABLE_PXP_TIMING
void pxpSendQueuedError(uint32_t packetTag, uint8_t commandType, uint8_t errorCode)
{
  if (pxpQueueResponseEndpointValid) {
    sendErrorTo(packetTag, 0, commandType, errorCode, pxpQueueResponseIp, pxpQueueResponsePort);
  }
}

void pxpExecuteScheduledEntry(const PxpScheduledEntry& entry)
{
  const uint32_t executeMs = millis();
  if (entry.latePolicy == PXP_LATE_DROP && pxpIsLate(entry.dueTimeMs, executeMs)) {
    DEBUG_PRINTF_P(PSTR("PXP scheduled drop late: tag=%lu cmd=0x%02X due=%lu now=%lu lateBy=%ld q=%u\n"),
                   (unsigned long)entry.packetTag, entry.commandType, (unsigned long)entry.dueTimeMs,
                   (unsigned long)executeMs, (long)(executeMs - entry.dueTimeMs), pxpScheduleCount);
    return;
  }

  const uint16_t payloadBytes = pxpArenaPayloadBytes(entry.offsetWords);
  if (!payloadBytes) {
    DEBUG_PRINTF_P(PSTR("PXP scheduled missing payload: tag=%lu cmd=0x%02X offset=%u q=%u\n"),
                   (unsigned long)entry.packetTag, entry.commandType, entry.offsetWords, pxpScheduleCount);
    pxpSendQueuedError(entry.packetTag, entry.commandType, PXP_ERROR_GENERIC);
    return;
  }

  PxpReader reader(pxpArenaPayload(entry.offsetWords), payloadBytes, false);
  uint8_t commandType;
  if (reader.read(commandType) != ReadStatus::Ok) {
    DEBUG_PRINTF_P(PSTR("PXP scheduled read cmd failed: tag=%lu expected=0x%02X payload=%u offset=%u q=%u\n"),
                   (unsigned long)entry.packetTag, entry.commandType, payloadBytes, entry.offsetWords, pxpScheduleCount);
    pxpSendQueuedError(entry.packetTag, entry.commandType, PXP_ERROR_LENGTH_MISMATCH);
    return;
  }

  uint32_t commandLen;
  if (!readVarUInt(reader, commandLen)) {
    DEBUG_PRINTF_P(PSTR("PXP scheduled read len failed: tag=%lu cmd=0x%02X payload=%u offset=%u q=%u\n"),
                   (unsigned long)entry.packetTag, commandType, payloadBytes, entry.offsetWords, pxpScheduleCount);
    pxpSendQueuedError(entry.packetTag, commandType, PXP_ERROR_LENGTH_MISMATCH);
    return;
  }

  PxpCommandReader command{reader, commandLen};
  PxpError error;
  error.command = commandType;

  switch (commandType) {
    case PXP_CMD_TARGET_RANGE:
    case PXP_CMD_TARGET_SPARSE:
    case PXP_CMD_TARGET_SPARSE_SEGMENTS: {
      if (commandLen > PXP_MAX_PACKET_SIZE || !copyCommandPayload(command, pxpVerifyScratch, commandLen)) {
        pxpSendQueuedError(entry.packetTag, commandType, command.compressionError ? PXP_ERROR_COMPRESSION : PXP_ERROR_LENGTH_MISMATCH);
        return;
      }
      uint8_t targetError = PXP_ERROR_BAD_ARGUMENTS;
      const TargetMode mode = commandType == PXP_CMD_TARGET_RANGE ? TargetMode::Range :
                              commandType == PXP_CMD_TARGET_SPARSE ? TargetMode::Sparse :
                              TargetMode::SparseSegments;
      if (!validateTargetPayload(mode, pxpVerifyScratch, commandLen, pxpScheduledTarget, targetError, pxpScheduledTargetData)) {
        DEBUG_PRINTF_P(PSTR("PXP scheduled target failed: tag=%lu cmd=0x%02X err=%u len=%lu due=%lu now=%lu q=%u\n"),
                       (unsigned long)entry.packetTag, commandType, targetError, (unsigned long)commandLen,
                       (unsigned long)entry.dueTimeMs, (unsigned long)millis(), pxpScheduleCount);
        pxpSendQueuedError(entry.packetTag, commandType, targetError);
      }
      return;
    }

    case PXP_CMD_PIXEL_DATA:
      if (pxpScheduledTarget.mode == TargetMode::None) {
        DEBUG_PRINTF_P(PSTR("PXP scheduled PixelData without target: tag=%lu len=%lu due=%lu now=%lu q=%u\n"),
                       (unsigned long)entry.packetTag, (unsigned long)commandLen,
                       (unsigned long)entry.dueTimeMs, (unsigned long)millis(), pxpScheduleCount);
      }
      if (pxpQueueResponseEndpointValid) realtimeIP = pxpQueueResponseIp;
      realtimeLock(realtimeTimeoutMs, REALTIME_MODE_PXP);
      if (!realtimeOverride && !handlePixelData(command, pxpScheduledTarget, pxpScheduledTargetData, error)) {
        DEBUG_PRINTF_P(PSTR("PXP scheduled PixelData failed: tag=%lu err=%u len=%lu targetMode=%u targetCount=%lu due=%lu now=%lu override=%u\n"),
                       (unsigned long)entry.packetTag, error.code, (unsigned long)commandLen,
                       (unsigned)pxpScheduledTarget.mode, (unsigned long)pxpScheduledTarget.count,
                       (unsigned long)entry.dueTimeMs, (unsigned long)millis(), realtimeOverride);
        pxpSendQueuedError(entry.packetTag, commandType, error.code);
      } else if (realtimeOverride && !command.skip()) {
        DEBUG_PRINTF_P(PSTR("PXP scheduled PixelData skip failed: tag=%lu err=%u len=%lu due=%lu now=%lu override=%u\n"),
                       (unsigned long)entry.packetTag, command.compressionError ? PXP_ERROR_COMPRESSION : PXP_ERROR_LENGTH_MISMATCH,
                       (unsigned long)commandLen, (unsigned long)entry.dueTimeMs, (unsigned long)millis(), realtimeOverride);
        pxpSendQueuedError(entry.packetTag, commandType, command.compressionError ? PXP_ERROR_COMPRESSION : PXP_ERROR_LENGTH_MISMATCH);
      }
      return;

    case PXP_CMD_VERIFY:
      if (pxpQueueResponseEndpointValid && !handleVerifyTo(command, entry.packetTag, error, pxpQueueResponseIp, pxpQueueResponsePort)) {
        pxpSendQueuedError(entry.packetTag, commandType, error.code);
      }
      return;

    default:
      pxpSendQueuedError(entry.packetTag, commandType, PXP_ERROR_UNSUPPORTED);
      return;
  }
}

void pxpRunScheduledDue()
{
  if (!pxpArenaReady) pxpArenaInit();
  uint8_t processed = 0;
  uint32_t now = millis();
  if (pxpScheduleCount && int32_t(now - pxpLastQueueTraceMs) > 1000) {
    pxpLastQueueTraceMs = now;
    DEBUG_PRINTF_P(PSTR("PXP queue pending: q=%u/%u nextDue=%lu now=%lu in=%ld firstCmd=0x%02X mode=%u\n"),
                   pxpScheduleCount, PXP_TIMING_QUEUE_ENTRIES, (unsigned long)pxpScheduleEntries[0].dueTimeMs,
                   (unsigned long)now, (long)(pxpScheduleEntries[0].dueTimeMs - now),
                   pxpScheduleEntries[0].commandType, realtimeMode);
  }
  while (pxpScheduleCount && processed < PXP_TIMING_QUEUE_ENTRIES && pxpTimeDue(pxpScheduleEntries[0].dueTimeMs, now)) {
    const PxpScheduledEntry entry = pxpScheduleEntries[0];
    pxpRemoveScheduledEntry(0);
    pxpExecuteScheduledEntry(entry);
    pxpArenaFree(entry.offsetWords);
    processed++;
    now = millis();
  }
  if (processed) {
    DEBUG_PRINTF_P(PSTR("PXP queue processed: count=%u remaining=%u now=%lu newData=%u mode=%u\n"),
                   processed, pxpScheduleCount, (unsigned long)now, pxpNewData, realtimeMode);
  }

  if (pxpScheduleCount && realtimeMode == REALTIME_MODE_PXP) {
    const uint32_t nextDue = pxpScheduleEntries[0].dueTimeMs;
    if (int32_t(nextDue - now) <= int32_t(realtimeTimeoutMs)) {
      const uint32_t keepUntil = nextDue + realtimeTimeoutMs;
      if (int32_t(keepUntil - realtimeTimeout) > 0) realtimeTimeout = keepUntil;
    }
  }
}
#endif

} // namespace

void pxpBeginUdp()
{
  pxpUdpConnected = false;
#ifdef WLED_ENABLE_PXP_TIMING
  pxpResetTiming();
#endif
  if (!pxpEnabled || !pxpPort) return;
  if (pxpPort == ntpLocalPort || pxpPort == udpPort || pxpPort == udpRgbPort ||
      pxpPort == udpPort2 || pxpPort == e131Port || pxpPort == DDP_DEFAULT_PORT) return;

  pxpUdpConnected = pxpUdp.begin(pxpPort);
}

void pxpHandleScheduled()
{
#ifdef WLED_ENABLE_PXP_TIMING
  pxpRunScheduledDue();
#endif
}

void pxpHandle()
{
  if (!pxpUdpConnected) return;

  const uint32_t receiveMs = millis();
  const int packetSize = pxpUdp.parsePacket();
  if (!packetSize) return;
  if (packetSize < 8 || packetSize > PXP_MAX_PACKET_SIZE) return;

  const int len = pxpUdp.read(pxpPacket, packetSize);
  if (len != packetSize) return;

  const uint16_t magicFlags = uint16_t(pxpPacket[0]) | (uint16_t(pxpPacket[1]) << 8);
  const uint32_t packetTag = uint32_t(pxpPacket[2]) | (uint32_t(pxpPacket[3]) << 8) |
                             (uint32_t(pxpPacket[4]) << 16) | (uint32_t(pxpPacket[5]) << 24);
  const uint16_t payloadLen = uint16_t(pxpPacket[6]) | (uint16_t(pxpPacket[7]) << 8);

  if ((magicFlags & ~0x000F) != PXP_MAGIC_VERSION) return;
  if (magicFlags & PXP_FLAG_RESERVED) return;
  if (packetSize != int(payloadLen) + 8) return;

#ifdef WLED_ENABLE_PXP_TIMING
  pxpQueueResponseIp = pxpUdp.remoteIP();
  pxpQueueResponsePort = pxpUdp.remotePort();
  pxpQueueResponseEndpointValid = true;
#endif

  const bool compressed = magicFlags & PXP_FLAG_COMPRESSED;
  PxpError error;
  uint32_t ordinal = 0;
  if (!processPacketCommands(pxpPacket + 8, payloadLen, compressed, packetTag, receiveMs, error, ordinal)) {
    sendError(packetTag, ordinal, error.command, error.code);
  }
}

#endif
