#ifndef LINK_WIRELESS_H
#define LINK_WIRELESS_H

// --------------------------------------------------------------------------
// // TODO: COMPLETE An SPI handler for the Link Port (Normal Mode, 32bits).
// --------------------------------------------------------------------------
// // TODO: Usage:
// - 1) Include this header in your main.cpp file and add:
//       LinkWireless* linkWireless = new LinkWireless();
// - 2) Initialize the library with:
//       linkWireless->activate();
// - 3) Exchange 32-bit data with the other end:
//       _________
//       // (this blocks the console indefinitely)
// - 4) Exchange data with a cancellation callback:
//       u32 data = linkGPIO->transfer(0x1234, []() {
//         u16 keys = ~REG_KEYS & KEY_ANY;
//         return keys & KEY_START;
//       });
// --------------------------------------------------------------------------
// considerations:
// - when using Normal Mode between two GBAs, use a GBC Link Cable!
// - only use the 2Mbps mode with custom hardware (very short wires)!
// --------------------------------------------------------------------------

#include <tonc_core.h>
#include <functional>  // TODO: REMOVE
#include <string>      // TODO: REMOVE
#include <vector>
#include "LinkGPIO.h"
#include "LinkSPI.h"

#define LINK_WIRELESS_PING_WAIT 50
#define LINK_WIRELESS_TRANSFER_WAIT 15
#define LINK_WIRELESS_TIMEOUT 100
#define LINK_WIRELESS_LOGIN_STEPS 9
#define LINK_WIRELESS_COMMAND_HEADER 0x9966
#define LINK_WIRELESS_RESPONSE_ACK 0x80
#define LINK_WIRELESS_DATA_REQUEST 0x80000000
#define LINK_WIRELESS_COMMAND_HELLO 0x10
#define LINK_WIRELESS_COMMAND_BROADCAST 0x16
#define LINK_WIRELESS_COMMAND_BROADCAST_READ 0x1d

const u16 LINK_WIRELESS_LOGIN_PARTS[] = {0x494e, 0x494e, 0x544e, 0x544e, 0x4e45,
                                         0x4e45, 0x4f44, 0x4f44, 0x8001};

class LinkWireless {
 public:
  std::function<void(std::string)> debug;  // TODO: REMOVE

  bool isActive() { return isEnabled; }

  // TODO: Add to docs
  bool activate() {
    if (!reset()) {
      deactivate();
      return false;
    }

    isEnabled = true;
    return true;
  }

  void deactivate() {
    isEnabled = false;
    stop();
  }

  bool broadcast(std::vector<u32> data) {
    // TODO: CHECK 6 ELEMENTS
    return sendCommand(LINK_WIRELESS_COMMAND_BROADCAST, data).success;
  }

  bool read(std::vector<u32>& data) {
    auto result = sendCommand(LINK_WIRELESS_COMMAND_BROADCAST_READ);
    data = result.responses;
    return result.success;
  }

  ~LinkWireless() {
    delete linkSPI;
    delete linkGPIO;
  }

 private:
  struct LoginMemory {
    u16 previousGBAData = 0xffff;
    u16 previousAdapterData = 0xffff;
  };

  struct CommandResult {
    bool success = false;
    std::vector<u32> responses = std::vector<u32>{};
  };

  LinkSPI* linkSPI = new LinkSPI();
  LinkGPIO* linkGPIO = new LinkGPIO();
  bool isEnabled = false;

  bool reset() {
    stop();
    return start();
  }

  bool start() {
    pingAdapter();
    linkSPI->activate(LinkSPI::Mode::MASTER_256KBPS);

    if (!login())
      return false;

    if (!sendCommand(LINK_WIRELESS_COMMAND_HELLO).success)
      return false;

    return true;
  }

  void stop() { linkSPI->deactivate(); }

  void pingAdapter() {
    linkGPIO->setMode(LinkGPIO::Pin::SO, LinkGPIO::Direction::OUTPUT);
    linkGPIO->setMode(LinkGPIO::Pin::SD, LinkGPIO::Direction::OUTPUT);
    linkGPIO->writePin(LinkGPIO::SD, true);
    wait(LINK_WIRELESS_PING_WAIT);
    linkGPIO->writePin(LinkGPIO::SD, false);
  }

  bool login() {
    LoginMemory memory;

    if (!exchangeLoginPacket(LINK_WIRELESS_LOGIN_PARTS[0], 0, memory))
      return false;

    for (u32 i = 0; i < LINK_WIRELESS_LOGIN_STEPS; i++) {
      if (!exchangeLoginPacket(LINK_WIRELESS_LOGIN_PARTS[i],
                               LINK_WIRELESS_LOGIN_PARTS[i], memory))
        return false;
    }

    return true;
  }

  bool exchangeLoginPacket(u16 data,
                           u16 expectedResponse,
                           LoginMemory& memory) {
    u32 packet = buildU32(~memory.previousAdapterData, data);
    u32 response = linkSPI->transfer(packet);

    if (msB32(response) != expectedResponse ||
        lsB32(response) != (u16)~memory.previousGBAData)
      return false;

    memory.previousGBAData = data;
    memory.previousAdapterData = expectedResponse;

    return true;
  }

  CommandResult sendCommand(u8 type,
                            std::vector<u32> params = std::vector<u32>{}) {
    CommandResult result;
    u16 length = params.size();
    u32 command = buildCommand(type, length);

    if (transfer(command) != LINK_WIRELESS_DATA_REQUEST)
      return result;

    for (auto& param : params) {
      if (transfer(param) != LINK_WIRELESS_DATA_REQUEST)
        return result;
    }

    u32 response = transfer(LINK_WIRELESS_DATA_REQUEST);
    u16 header = msB32(response);
    u16 data = lsB32(response);
    u8 responses = msB16(data);
    u8 ack = lsB16(data);
    if (header != LINK_WIRELESS_COMMAND_HEADER)
      return result;
    if (ack != type + LINK_WIRELESS_RESPONSE_ACK)
      return result;

    for (u32 i = 0; i < responses; i++)
      result.responses.push_back(transfer(LINK_WIRELESS_DATA_REQUEST));

    result.success = true;
    return result;
  }

  u32 buildCommand(u8 type, u8 length = 0) {
    return buildU32(LINK_WIRELESS_COMMAND_HEADER, buildU16(length, type));
  }

  u32 transfer(u32 data) {
    wait(LINK_WIRELESS_TRANSFER_WAIT);

    u32 lines = 0;
    u32 vCount = REG_VCOUNT;

    return linkSPI->transfer(data, [&lines, &vCount]() {
      if (REG_VCOUNT != vCount) {
        lines++;
        vCount = REG_VCOUNT;
      }

      return lines > LINK_WIRELESS_TIMEOUT;
    });
  }

  void wait(u32 verticalLines) {
    u32 lines = 0;
    u32 vCount = REG_VCOUNT;

    while (lines < verticalLines) {
      if (REG_VCOUNT != vCount) {
        lines++;
        vCount = REG_VCOUNT;
      }
    };
  }

  u32 buildU32(u16 msB, u16 lsB) { return (msB << 16) | lsB; }
  u16 buildU16(u8 msB, u8 lsB) { return (msB << 8) | lsB; }
  u16 msB32(u32 value) { return value >> 16; }
  u16 lsB32(u32 value) { return value & 0xffff; }
  u8 msB16(u16 value) { return value >> 8; }
  u8 lsB16(u16 value) { return value & 0xff; }
};

extern LinkWireless* linkWireless;

#endif  // LINK_WIRELESS_H
