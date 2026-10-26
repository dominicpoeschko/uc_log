#pragma once

// The member functions of BasicJLinkRttReader (JLinkRttReader.hpp holds the class with their declarations), its
// Session, and the protocols with state of their own (printer/reader/: the announced resets, the flash gate, the boot
// count, the target ops, the halt capture): included only where the reader is instantiated - printer/ReaderJLink.cpp
// for the J-Link, tests/rtt_reader/rtt_reader_tests.cpp for the host tests' fake transport - so a program that only
// uses the reader compiles none of it. What stays here is the session loop: connect, serve a flash or reset, wait for
// the block, run a session pass by pass, end it, and the recovery ladder.

#include "uc_log/printer/BootLedger.hpp"
#include "uc_log/printer/CortexM.hpp"
#include "uc_log/printer/DecodeWorker.hpp"
#include "uc_log/printer/DirectRtt.hpp"
#include "uc_log/printer/JLinkRttReader.hpp"
#include "uc_log/printer/Le32.hpp"
#include "uc_log/printer/MessageLog.hpp"
#include "uc_log/printer/RttChannel.hpp"
#include "uc_log/printer/RttControlBlock.hpp"
#include "uc_log/printer/RttSource.hpp"
#include "uc_log/printer/reader/AnnouncedResets.hpp"
#include "uc_log/printer/reader/BootCount.hpp"
#include "uc_log/printer/reader/CaptureHalt.hpp"
#include "uc_log/printer/reader/FlashGate.hpp"
#include "uc_log/printer/reader/TargetOps.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <variant>

// the reader's protocols that keep state across sessions; created with the reader, gone after its thread
template<typename TransportT>
struct uc_log::printer::reader::Parts {
    FlashGate<TransportT> gate;
    BootCount<TransportT> boots;
    TargetOps<TransportT> ops;

    Parts(MessageLog& messages                              UC_LOG_LIFETIMEBOUND,
          ReaderSink const&                                 sink,
          ImageSource const&                                image,
          ReaderOptions const&                              options,
          typename FlashGate<TransportT>::HealthLook const& healthLook)
      : gate{messages,
             options.flashReady,
             options.flashWindow,
             healthLook}
      , boots{messages,
              image.loggedBoots,
              sink.unreadBoots,
              sink.bootCountRestart}
      , ops{gate} {}
};

template<typename TransportT>
struct BasicJLinkRttReader<TransportT>::LogChannel {
    std::uint32_t               upIndex;   // also the channel it is logged as
    uc_log::printer::RttChannel rtt;
};

// What lives for one session: the announced-reset block, the RTT source, the log channels and their decoder, the read
// chunks, the loop's clocks. Built when the session opens, gone when it ends.
template<typename TransportT>
class BasicJLinkRttReader<TransportT>::Session {
public:
    Session(BasicJLinkRttReader& reader     UC_LOG_LIFETIMEBOUND,
            TransportT& transport           UC_LOG_LIFETIMEBOUND,
            std::unordered_map<std::uint16_t,
                               std::string> strings,
            std::optional<std::uint32_t>    announceBlock,
            std::stop_token const&          stoken)
      : r{reader}
      , jlink{transport}
      , strings_{std::move(strings)}
      , announce{reader.sink_.messages,
                 transport,
                 announceBlock,
                 stoken}
      , blockInfo{reader.imageSource.block()}
      , rtt{reader.openRttSource(transport,
                                 blockInfo)}
      , started{rtt->started()}
      , channelMap{uc_log::printer::buildRttChannelMap(
          *rtt,
          started,
          [&reader](std::string_view m) { reader.say(m); },
          [&reader](std::string_view m) { reader.sayError(m); })}
      , epochFenced{rtt->sessionEpoch().has_value()} {
        if(r.duplexBridge.configure) { r.duplexBridge.configure(channelMap.duplexChannels); }
        auto const frameCheck = r.frameCheckOfImage();
        for(auto const& log : channelMap.logChannels) {
            channels.push_back({log.upIndex, uc_log::printer::RttChannel{}});
            channels.back().rtt.frameCheck = frameCheck;
        }
        takeHandover();
        allocateChunks();
    }

    Session(Session const&)            = delete;
    Session& operator=(Session const&) = delete;

    // The session died by an exception (a probe failure): what was read is decoded, and the unfinished bytes of
    // each channel - the head of a record whose tail is still in the target - go to the next session with
    // this session's RTT epoch.
    void keepUnfinished() noexcept {
        try {
            decoder.barrier();
        } catch(
          ...) { /* ignored: the session ends on an exception already; the carry below is kept */
        }
        for(auto const& ch : channels) {
            if(!ch.rtt.buffer.empty()) { r.handover_.carry[ch.upIndex] = ch.rtt.buffer; }
        }
        if(!r.handover_.carry.empty()) { r.handover_.carryEpoch = rtt->sessionEpoch(); }
    }

    // The session's first bytes: its boot logged and is read (reader::BootCount) - the boots before it that logged
    // and nobody read are said now, ahead of this boot's lines.
    void noteBootRead() {
        bootNoted = true;
        if(auto const count = r.parts_->boots.bootRead(jlink)) { bootCount = count; }
    }

    // At the session's end: a boot noted as read whose bytes gave no line (the session ended on a probe failure
    // before a whole record, the bytes dropped) was not read after all - a boot of the watchdog loop the printer
    // then replaced went uncounted.
    void settleUnprinted() {
        printedAny = printedAny || decoder.takePrinted();
        if(bootNoted && !printedAny && bootCount) { r.parts_->boots.notRead(*bootCount); }
    }

    // Hands what a read took from each log channel's ring to the decoder - an empty item too: the decode times a
    // stuck frame out (RttChannel::drain) and decodes what a channel already holds. A probe failure the read kept
    // is thrown after its bytes are with the decoder: the session ends as on any.
    std::size_t decodeRead(std::span<std::span<std::byte> const> read,
                           bool                                  haltedRecently) {
        std::size_t bytes = 0;
        for(std::size_t c = 0; c != channels.size(); ++c) {
            auto const upIndex = channels[c].upIndex;
            auto const data    = upIndex < read.size() ? read[upIndex] : std::span<std::byte>{};
            bytes += data.size();
            decoder.push({c, std::vector<std::byte>(data.begin(), data.end()), haltedRecently});
        }
        r.rttBytes_ += bytes;
        if(bytes != 0 && !bootNoted) { noteBootRead(); }
        if(auto const failure = rtt->takeFailure()) { std::rethrow_exception(failure); }
        return bytes;
    }

    // What each log ring holds now, read whole and decoded after everything read before it (one read per ring
    // ending on a record boundary: a target that floods on does not keep a stopping printer reading); the
    // bytes it read and the bytes of unfinished lines left in the channels.
    std::pair<std::size_t,
              std::size_t>
    drainAll(bool haltedRecently) {
        auto const read = decodeRead(rtt->drain(logOuts), haltedRecently);
        decoder.barrier();
        std::size_t unfinished{};
        for(auto const& ch : channels) { unfinished += ch.rtt.buffer.size(); }
        return {read, unfinished};
    }

    BasicJLinkRttReader&                           r;
    TransportT&                                    jlink;
    std::unordered_map<std::uint16_t, std::string> strings_;
    // armed first: before the RTT source opens, as before the session (the firmware's wait for the printer)
    uc_log::printer::reader::AnnouncedResets<TransportT>    announce;
    uc_log::printer::RttBlockInfo                           blockInfo;
    std::unique_ptr<uc_log::printer::RttSource<TransportT>> rtt;
    Status                                                  started;
    uc_log::printer::RttChannelMap                          channelMap;
    bool                                                    epochFenced;
    std::vector<LogChannel>                                 channels;
    // A chunk per up ring the session reads, by its index: a log ring's holds the whole ring (the drain takes a
    // ring whole), a duplex ring's what one pass passes on. `passOuts`: what a pass reads, `logOuts`: what a
    // drain reads.
    std::vector<std::vector<std::byte>>    upChunks;
    std::vector<std::span<std::byte>>      passOuts;
    std::vector<std::span<std::byte>>      logOuts;
    std::array<std::byte, duplexChunkSize> duplexOut{};
    // the loop's clocks
    Clock::time_point lastMessage{Clock::now()};
    Clock::time_point lastHaltDetected{};
    Clock::time_point lastHaltPoll{};
    bool              lastPassRead{};   // the pass before read bytes (busyHaltPoll)
    Clock::time_point nextStallCheck{Clock::now() + stallCheckAfter};
    bool              wasHalted{};
    bool              quietWarned{};
    bool              bootNoted{};   // the session's boot is noted as read (noteBootRead)
    std::optional<uc_log::printer::BootLedger::Count>
         bootCount;      // its count of boots that logged then
    bool printedAny{};   // the decoder gave a line in this session
    // the bytes of each pass are decoded on a thread of their own while the next pass reads
    // (printer/DecodeWorker.hpp); last: it goes before the channels it decodes into, and it is handed an item only
    // after the constructor filled them
    uc_log::printer::DecodeWorker decoder{[this](uc_log::printer::DecodeWorker::Item& item) {
        auto& [upIndex, channel] = channels[item.slot];
        if(!item.bytes.empty()) { channel.append(item.bytes); }
        return channel.drain(
          r.sink_.entry,
          upIndex,
          strings_,
          [this](std::string_view m, remote_fmt::ParseError kind) {
              if(kind == remote_fmt::ParseError::format) {
                  r.sink_.messages.formatError(m);
              } else {
                  r.sayError(m);
              }
          },
          item.haltedRecently);
    }};

private:
    // The session before died on a probe failure holding the head of a record whose tail is still in the target:
    // it goes in front of this session's first bytes - only if the target has not booted since (the same RTT
    // epoch); otherwise its loss is said with its size.
    void takeHandover() {
        auto& carry = r.handover_.carry;
        if(carry.empty()) { return; }
        // the same boot, and its rings read on where the session before stopped (not skipped: DirectRtt::skipUnread)
        bool const  sameBoot = rtt->sessionEpoch() && r.handover_.carryEpoch
                            && *rtt->sessionEpoch() == *r.handover_.carryEpoch
                            && !rtt->staleSkipped();
        std::size_t dropped  = 0;
        for(auto const& [upIndex, bytes] : carry) {
            auto const ch = std::ranges::find(channels, upIndex, &LogChannel::upIndex);
            if(sameBoot && ch != channels.end()) {
                ch->rtt.append(bytes);
            } else {
                dropped += bytes.size();
            }
        }
        if(dropped != 0) {
            r.say(
              fmt::format("{} byte{} of an unfinished line dropped: the session before ended on a "
                          "probe failure{}",
                          dropped,
                          dropped == 1 ? "" : "s",
                          rtt->sessionEpoch() ? " and the target booted again since"
                                              : " (no RTT epoch to tell whether the target booted "
                                                "since)"));
        }
        carry.clear();
        r.handover_.carryEpoch.reset();
    }

    void allocateChunks() {
        upChunks.resize(static_cast<std::size_t>(std::max(started.numUpBuffers, 0)));
        passOuts.resize(upChunks.size());
        logOuts.resize(upChunks.size());
        for(auto const& ch : channels) {
            if(ch.upIndex >= upChunks.size()) { continue; }
            upChunks[ch.upIndex].resize(
              std::max<std::size_t>(rttBufferChunkSize, rtt->upSize(ch.upIndex)));
            passOuts[ch.upIndex] = logOuts[ch.upIndex] = std::span{upChunks[ch.upIndex]};
        }
        for(auto const& dc : channelMap.duplexChannels) {
            if(!dc.upIndex || *dc.upIndex >= upChunks.size() || !r.duplexBridge.sendToClient) {
                continue;
            }
            upChunks[*dc.upIndex].resize(duplexChunkSize);
            passOuts[*dc.upIndex] = std::span{upChunks[*dc.upIndex]};
        }
    }
};

template<typename TransportT>
BasicJLinkRttReader<TransportT>::BasicJLinkRttReader(uc_log::printer::ReaderConnection connection,
                                                     uc_log::printer::ReaderSink       sink,
                                                     uc_log::printer::ImageSource      image,
                                                     uc_log::printer::ReaderOptions    options,
                                                     uc_log::printer::DuplexBridge duplexBridge_)
  : host{std::move(connection.host)}
  , device{std::move(connection.device)}
  , speed{connection.speed}
  , probe{std::move(connection.probe)}
  , sink_{std::move(sink)}
  , imageSource{std::move(image)}
  , readerOptions{std::move(options)}
  , duplexBridge{std::move(duplexBridge_)}
  , health_{[this](std::string_view m) { sayError(m); },
            [this](std::string_view m) { sayTool(m); },
            readerOptions.park,
            readerOptions.stall,
            readerOptions.stallFirstStep}
  , parts_{std::make_unique<uc_log::printer::reader::Parts<TransportT>>(
      sink_.messages,
      sink_,
      imageSource,
      readerOptions,
      [this](TransportT& jlink) { return healthBeforeFlashWait(jlink); })}
  , thread{[this](std::stop_token const& stoken) { run(stoken); }} {
    static_assert(busyHaltPoll + std::chrono::milliseconds{30}
                  < uc_log::printer::RttChannel::frameTimeout);
}

// out of line: Parts is complete only here
template<typename TransportT>
BasicJLinkRttReader<TransportT>::~BasicJLinkRttReader() = default;

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::say(std::string_view text) const -> void {
    sink_.messages.status(text);
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::sayError(std::string_view text) const -> void {
    sink_.messages.error(text);
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::sayTool(std::string_view text) const -> void {
    sink_.messages.tool(text);
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::sayToolError(std::string_view text) const -> void {
    sink_.messages.toolError(text);
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::controlBlockPresent(TransportT&   jlink,
                                                          std::uint32_t address) -> bool {
    std::array<std::byte, uc_log::printer::rtt_block::id.size()> raw{};
    try {
        jlink.readMemory(address, raw);
    } catch(std::exception const&) { return false; }
    return uc_log::printer::rtt_block::View{raw}.hasId();
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::checkTargetAnswers(TransportT&        jlink,
                                                         Clock::time_point& lastAnswer) -> void {
    if(uc_log::printer::tryReadWord(jlink, uc_log::printer::cortex_m::cpuid)) {
        lastAnswer = Clock::now();
        return;
    }
    if(Clock::now() - lastAnswer > targetSilentLimit) {
        throw TargetSilent{
          fmt::format("no answer from the target for {} s",
                      std::chrono::duration_cast<std::chrono::seconds>(targetSilentLimit).count())};
    }
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::awaitTargetAnswers(TransportT&            jlink,
                                                         std::stop_token const& stoken) -> void {
    auto lastAnswer = Clock::now();
    while(!stoken.stop_requested()
          && !uc_log::printer::tryReadWord(jlink, uc_log::printer::cortex_m::cpuid))
    {
        checkTargetAnswers(jlink, lastAnswer);
        Clock::sleep_for(std::chrono::milliseconds{50});
    }
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::awaitControlBlockAfterReset(TransportT&            jlink,
                                                                  std::stop_token const& stoken)
  -> void {
    if(!handover_.awaitBlock) { return; }
    handover_.awaitBlock  = false;
    auto const endedEpoch = std::exchange(handover_.endedEpoch, std::nullopt);
    auto const epochAt    = rttEpochAddress();
    auto const newBoot    = [&](std::uint32_t at) {
        if(!controlBlockPresent(jlink, at)) { return false; }
        if(!endedEpoch || !epochAt) { return true; }
        auto const epoch = uc_log::printer::tryReadWord(jlink, *epochAt);
        return epoch && *epoch != *endedEpoch;
    };
    auto address = imageSource.block().address;
    if(address == 0) { return; }
    bool       told            = false;
    auto const waitStart       = Clock::now();
    auto       nextHealthCheck = waitStart + healthCheckPeriod;
    auto       lastAnswer      = waitStart;
    health_.beginWait();
    while(!stoken.stop_requested() && !targetResetFlag && !flashFlag && !jlinkResetFlag
          && !newBoot(address))
    {
        checkTargetAnswers(jlink, lastAnswer);
        if(!told) {
            say(
              fmt::format("waiting for the RTT control block at {:#010x}: the "
                          "target runs something else (its boot ROM?)",
                          address));
            told = true;
        }
        if(Clock::now() > nextHealthCheck) {
            nextHealthCheck = Clock::now() + healthCheckPeriod;
            auto const action
              = health_.checkWhileWaiting(jlink, waitStart, parts_->ops.haltRequested);
            if(action == Health::Action::printerReset) {
                targetResetFlag = true;
                break;
            }
            if(action == Health::Action::recovered || action == Health::Action::resetLine) {
                // the recovery reset the chip: the DLL's connection may not see the target again until it connects
                // anew, and a connect right away can land in the new boot's boot ROM window - off the probe a moment
                // first; the next connection waits for the new boot's block
                handover_.awaitBlock = true;
                afterRecovery(action);
                throw std::runtime_error{"the target was recovered: connecting again to see it"};
            }
            // the build's files may change meanwhile (a rebuild, or back to what the target runs): the block is
            // where the map says now - waiting on at the old map's address waited for good
            if(auto const now = imageSource.block().address; now != address) {
                if(now == 0) { break; }
                address = now;
                told    = false;
            }
        }
        parts_->ops.serve(jlink);
        Clock::sleep_for(controlBlockPoll);
    }
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::serviceProbeListRequest() -> void {
    if(!probeListRequest.exchange(false)) { return; }
    try {
        std::vector<Probe> found;
        for(auto const& p : TransportT::listProbes()) {
            found.push_back(Probe{p.serialNumber, p.product, p.nickName, p.onUsb});
        }
        sayTool(fmt::format("found {} J-Link probe{}", found.size(), found.size() == 1 ? "" : "s"));
        {
            std::scoped_lock<std::mutex> lock{probesMutex};
            probes_ = std::move(found);
        }
        probesVersion_.fetch_add(1, std::memory_order_release);
    } catch(std::exception const& e) {
        sayToolError(fmt::format("listing J-Link probes failed: {}", e.what()));
    }
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::openRttSource(TransportT&                          jlink,
                                                    uc_log::printer::RttBlockInfo const& blockInfo)
  -> std::unique_ptr<uc_log::printer::RttSource<TransportT>> {
    if(readerOptions.directRtt) {
        if(auto direct = uc_log::printer::DirectRtt<TransportT>::open(jlink,
                                                                      blockInfo.address,
                                                                      blockInfo.totalBuffers,
                                                                      rttEpochAddress()))
        {
            sayTool(fmt::format("RTT read by the printer at {:#010x}", blockInfo.address));
            if(auto const skipped = direct->staleSkipped()) {
                say(fmt::format(
                  "the RTT epoch stays odd ({:#x}): a boot that died in its init - {} byte{} in "
                  "the rings skipped unread (they may be the ended boot's tail; never read from "
                  "the middle of a record)",
                  direct->sessionEpoch(),
                  *skipped,
                  *skipped == 1 ? "" : "s"));
            }
            return std::make_unique<uc_log::printer::DirectSource<TransportT>>(
              jlink,
              std::move(*direct),
              parts_->gate.read(jlink));
        }
    }
    sayTool("RTT read by the J-Link DLL");
    return std::make_unique<uc_log::printer::DllRtt<TransportT>>(
      jlink,
      blockInfo.address,
      blockInfo.totalBuffers,
      [this](std::string_view m) { sayError(m); });
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::rttEpochAddress() const -> std::optional<std::uint32_t> {
    return imageSource.rttEpoch ? imageSource.rttEpoch() : std::nullopt;
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::frameCheckOfImage() const -> remote_fmt::FrameCheck {
    return imageSource.frameCheck ? imageSource.frameCheck() : remote_fmt::FrameCheck::either;
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::decodeAllowed(TransportT& jlink) -> bool {
    if(!imageSource.decodeGate) { return true; }
    return imageSource.decodeGate(parts_->gate.read(jlink));
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::healthBeforeFlashWait(TransportT& jlink) -> bool {
    auto const action = health_.checkWhileWaiting(jlink, Clock::now(), parts_->ops.haltRequested);
    if(action == Health::Action::printerReset) {
        targetResetFlag = true;
        return true;
    }
    if(action != Health::Action::none) {
        afterRecovery(action);
        return true;
    }
    return false;
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::run(std::stop_token const& stoken) -> void {
    while(!stoken.stop_requested()) {
        sayTool("start jlink");
        std::optional<std::chrono::milliseconds>
          announcedPause;   // the session ended for an announced reset
        try {
            takePendingConnection();
            TransportT jlink{
              device,
              speed,
              typename TransportT::Connection{host, 19020, probe},
              [this](std::string_view m) { sayTool(m); },
              [this](std::string_view m) { sayToolError(m); }
            };
            bool const resetLine
              = std::exchange(resetPinOnce_, false);   // this connection's reset: the line
            jlink.setResetType(resetLine ? resetTypeLine
                                         : parts_->ops.resetType.load(std::memory_order_relaxed));
            jlink.setPreResetCommands(readerOptions.preResetCommands);
            if(!resetLine) {
                awaitTargetAnswers(jlink, stoken);
            }   // the reset line is tried on a silent one
            if(serviceFlashAndReset(jlink)) { continue; }
            awaitControlBlockAfterReset(jlink, stoken);
            // our own reset (or flash) set it: only a reset after this point is from outside
            (void)parts_->gate.resetSinceLastLook(jlink);
            announcedPause = runSession(jlink, stoken);
        } catch(TargetSilent const& e) {
            escalateSilentTarget(e.what());
            Clock::sleep_for(std::chrono::milliseconds{1000});
        } catch(std::exception const& e) {
            sayToolError(fmt::format("caught {}", e.what()));
            Clock::sleep_for(std::chrono::milliseconds{1000});
        } catch(...) {
            // a non-std exception (e.g. from a callback) must reconnect, not terminate
            sayToolError("caught unknown exception");
            Clock::sleep_for(std::chrono::milliseconds{1000});
        }
        Status notRunning{};
        notRunning.isRunning = 0;
        status               = notRunning;
        parts_->ops.halted = false;   // haltRequested stays: a core a client halted is still halted
        parts_->ops.failAll("the target is not connected");
        sayTool("stopped jlink");
        if(announcedPause) { pauseForAnnouncedReset(stoken, *announcedPause); }
        if(std::exchange(offProbeOnce_, false)) {
            sayTool(
              fmt::format("off the probe {} ms after the recovery: the new boot runs its boot "
                          "ROM untouched",
                          recoveryOffProbe.count()));
            Clock::sleep_for(recoveryOffProbe);
        }
    }
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::afterRecovery(typename Health::Action action) -> void {
    if(action == Health::Action::resetLine) {
        resetPinOnce_   = true;
        targetResetFlag = true;
        return;
    }
    if(action == Health::Action::recovered) { offProbeOnce_ = true; }
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::escalateSilentTarget(std::string_view what) -> void {
    handover_.awaitBlock
      = true;   // the next connection waits (and watches the silence) before a session
    auto const n = ++silentConnections_;
    if(n < silentResetAfter) {
        sayError(fmt::format("{}: the probe connects again ({} of {} before a reset)",
                             what,
                             n,
                             silentResetAfter - 1));
    } else if(n == silentResetAfter) {
        sayError(fmt::format("{} after {} new connections: resetting it through its reset line",
                             what,
                             n - 1));
        resetPinOnce_   = true;
        targetResetFlag = true;
    } else {
        sayError(
          fmt::format("{} after a reset through its reset line: power-cycle it (unplug its USB) - "
                      "the printer keeps trying",
                      what));
    }
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::takePendingConnection() -> void {
    {
        std::scoped_lock<std::mutex> lock{connectionMutex};
        if(pendingConnection) {
            host  = std::move(pendingConnection->host);
            probe = std::move(pendingConnection->probe);
            pendingConnection.reset();
        }
    }
    if(readerOptions.beforeConnect) { readerOptions.beforeConnect(); }
    jlinkResetFlag = false;
    serviceProbeListRequest();
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::serviceFlashAndReset(TransportT& jlink) -> bool {
    if(flashFlag.load() || targetResetFlag.load()) { parts_->boots.beforeReplacing(jlink); }
    bool restart = false;
    if(flashFlag.exchange(false)) { restart = flashTarget(jlink); }
    if(targetResetFlag.exchange(false)) { restart = resetTargetNow(jlink) || restart; }
    return restart;
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::flashTarget(TransportT& jlink) -> bool {
    flashing_           = true;
    auto const ramImage = ramImageStart();
    try {
        if(ramImage) {
            // no reset after the download: it would boot flash, not the image
            if(!*ramImage) { throw std::runtime_error{ramImage->error()}; }
            sayTool("flashing RAM image");
            jlink.flashRamImage(imageSource.hexFile(), **ramImage);
            sayTool("flashing RAM image succeeded, started it");
        } else {
            sayTool("resetting target");
            jlink.resetTarget();
            sayTool("resetting target succeeded");
            sayTool("flashing target");
            jlink.flash(imageSource.hexFile());
            sayTool("flashing target succeeded");
        }
    } catch(std::exception const& e) {
        flashing_ = false;
        // a target whose flash interface is stalled (an RP2350 QMI left busy by its boot ROM) fails every download
        // ("RAMCode did not respond") until the package's recovery has run: run it and download again, not
        // counted as an attempt
        if(!ramImage && health_.recoverStalledFlash(jlink, e.what())) {
            flashFlag = true;
            throw;
        }
        // a J-Link with a second client fails about every other download; a flash interface stuck where no
        // register shows it fails every one until the chip has booted once more (prepareRetry)
        if(++flashAttempt >= flashAttempts) {
            flashAttempt = 0;
            setOutcome(flashOutcome_, std::string{e.what()}, sessionCount());
        } else {
            if(!ramImage) { prepareRetry(jlink, e.what()); }
            flashFlag = true;
        }
        throw;
    }
    flashAttempt = 0;
    health_.flashDone();
    parts_->boots.restartAfterDownload();
    flashing_                 = false;
    parts_->ops.haltRequested = false;   // the image starts anew
    if(!ramImage) { targetResetFlag = true; }
    setOutcome(flashOutcome_, std::string{}, sessionCount());
    return true;
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::prepareRetry(TransportT&      jlink,
                                                   std::string_view why) -> void {
    health_.recoverBeforeRetry(jlink, why);
    try {
        jlink.resetTarget();
        jlink.go();
        Clock::sleep_for(retryBootTime);
    } catch(std::exception const& e) {
        sayToolError(
          fmt::format("the reset before the next download attempt failed: {}", e.what()));
    }
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::resetTargetNow(TransportT& jlink) -> bool {
    // every resetAndWait() numbered up to here is served by this reset
    auto const  covered  = resetRequests_.load();
    auto const  ramImage = ramImageStart();
    std::string error;
    bool        reset = false;
    if(ramImage && !*ramImage) {
        // nothing to start: told, not retried (the files will not change)
        error = ramImage->error();
        sayToolError(fmt::format("not reset: {}", error));
    } else {
        // a reset asked for through the reset pin (resetAndWait's `line`): one pulse on the pin (JLink::
        // pulseResetPin: J-Link reset type 2 pulses an RP2350's RUN three times); a RAM image is loaded again
        // with reset type 2 for this one, then the configured type again
        bool const line = resetLineRequested_.exchange(false);
        sayTool(
          ramImage
            ? fmt::format("resetting the RAM image{}: loading {} again and starting it",
                          line ? " through its reset line" : "",
                          imageSource.hexFile())
            : std::string{line ? "resetting target through its reset line" : "resetting target"});
        if(line && ramImage) { jlink.setResetType(resetTypeLine); }
        try {
            if(ramImage) {
                jlink.flashRamImage(imageSource.hexFile(), **ramImage);
            } else if(line) {
                jlink.pulseResetPin();
            } else {
                jlink.resetTarget();
            }
        } catch(...) {
            if(line) {
                jlink.setResetType(parts_->ops.resetType.load(std::memory_order_relaxed));
                resetLineRequested_ = true;   // retried as asked
            }
            targetResetFlag = true;
            throw;
        }
        if(line && ramImage) {
            jlink.setResetType(parts_->ops.resetType.load(std::memory_order_relaxed));
        }
        sayTool("resetting target succeeded");
        parts_->ops.haltRequested = false;   // the core runs again
        reset                     = true;
    }
    std::scoped_lock<std::mutex> const lock{resetOutcome_.mutex};
    resetOutcome_.serial         = covered;
    resetOutcome_.sessionsBefore = sessionCount();
    resetOutcome_.error          = std::move(error);
    return reset;
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::runSession(TransportT&            jlink,
                                                 std::stop_token const& stoken)
  -> std::optional<std::chrono::milliseconds> {
    auto strings = imageSource.catalog();
    runSessionStart(jlink);
    if(!decodeAllowed(jlink)) {
        holdAtGate(jlink, stoken);
        return std::nullopt;
    }
    // the announced-reset block is armed by the session, only after the session start's flash reads
    // (reader::AnnouncedResets); its address from the map file, once
    Session s{*this,
              jlink,
              std::move(strings),
              imageSource.announcedReset ? imageSource.announcedReset() : std::nullopt,
              stoken};
    if constexpr(requires { jlink.serialNumber(); }) { probeSerial_ = jlink.serialNumber(); }
    parts_->boots.sessionStart(jlink);
    sessionCount_.fetch_add(1, std::memory_order_release);
    // not health_.logRead() here: a session starts on the old block after every reconnect, and the stall ladder must
    // go on across them - only lines read set it back (readPass)
    silentConnections_ = 0;   // the target answers again
    std::optional<SessionEnd> end;
    try {
        while(!end) { end = readPass(s, stoken); }
    } catch(...) {
        s.keepUnfinished();
        s.settleUnprinted();
        throw;
    }
    auto const pause = endSession(s, *end, stoken);
    s.settleUnprinted();
    return pause;
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::runSessionStart(TransportT& jlink) -> void {
    if(!imageSource.onSessionStart) { return; }
    imageSource.onSessionStart(parts_->gate.read(jlink));
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::holdAtGate(TransportT&            jlink,
                                                 std::stop_token const& stoken) -> void {
    say(
      "not decoding: the target runs another image than the build's files - the log waits in "
      "the "
      "target's rings until it is flashed (or the files match)");
    bool       opened          = false;
    bool       recovered       = false;
    auto const gateStart       = Clock::now();
    auto       nextHealthCheck = gateStart + healthCheckPeriod;
    auto       lastAnswer      = gateStart;
    health_.beginWait();
    while(!stoken.stop_requested() && !jlinkResetFlag && !targetResetFlag && !flashFlag) {
        checkTargetAnswers(jlink, lastAnswer);
        parts_->ops.serve(jlink);
        serviceProbeListRequest();
        if(parts_->gate.resetSinceLastLook(jlink)) {
            break;
        }   // a new boot: a new session checks it again
        // "another image" may be a parked chip whose flash the boot ROM left unmapped
        if(Clock::now() > nextHealthCheck) {
            nextHealthCheck = Clock::now() + healthCheckPeriod;
            auto const action
              = health_.checkWhileWaiting(jlink, gateStart, parts_->ops.haltRequested);
            if(action != Health::Action::none) {
                targetResetFlag = targetResetFlag || action == Health::Action::printerReset;
                afterRecovery(action);
                recovered = true;
                break;
            }
        }
        Clock::sleep_for(decodeGatePoll);
        if(decodeAllowed(jlink)) {
            opened = true;
            break;
        }
    }
    if(opened) { say("the build's files match the target now: decoding"); }
    handover_.awaitBlock = recovered;   // reset: the new boot's block is waited for
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::readPass(Session&               s,
                                               std::stop_token const& stoken)
  -> std::optional<SessionEnd> {
    if(stoken.stop_requested() || jlinkResetFlag || targetResetFlag || flashFlag) {
        return SessionEnd::stopOrReplace;
    }
    bool const haltedRecently = Clock::now() < s.lastHaltDetected + haltGracePeriod;
    bool const haltPollDue
      = Clock::now() >= s.lastHaltPoll + (s.lastPassRead ? busyHaltPoll : haltPoll);
    if(haltPollDue) { s.lastHaltPoll = Clock::now(); }
    if(auto const end = pollAnnouncedReset(s, haltedRecently)) { return end; }
    // DHCSR.S_RESET_ST: every pass - or, where the RTT epoch fences what a pass reads, only in the passes that
    // poll the halt state (a reset then shows in the next pass that reads bytes, DirectRtt::readAll; this poll
    // ends a session whose rings stay empty): one probe round trip a pass less. Read before the halt poll of the
    // same pass: the J-Link DLL's own DHCSR reads may clear the bit.
    if((!s.epochFenced || haltPollDue) && parts_->gate.resetSinceLastLook(s.jlink)) {
        // Someone else reset the target (picotool, a watchdog, the button). SEGGER's RTT reader carries on across
        // it and, once the new image or the boot ROM rewrites the control block, reads an old lap of a ring: take
        // what is there, then start over as after a reset of our own.
        sayNewBoot(s,
                   "the target was reset from outside the printer",
                   "of a line cut off by the reset",
                   haltedRecently);
        return SessionEnd::newBoot;
    }
    auto const passed = s.rtt->readPass(s.passOuts);
    s.lastPassRead    = s.decodeRead(passed, haltedRecently) != 0;
    bool morePending  = s.lastPassRead;
    for(std::size_t i = 0; i != s.passOuts.size(); ++i) {   // a ring that filled its chunk has more
        morePending
          = morePending || (!s.passOuts[i].empty() && passed[i].size() == s.passOuts[i].size());
    }
    if(s.decoder.takePrinted()) {
        s.printedAny  = true;
        s.lastMessage = Clock::now();
        s.quietWarned = false;
        health_.logRead();
    }
    if(s.rtt->reinitSeen()) {
        // A boot re-initialised the RTT block (a reset nobody announced: the RTT epoch moved). What was read
        // across it is dropped; the next session starts at the new block's first byte.
        sayNewBoot(s,
                   "the target booted again (its RTT block was re-initialised)",
                   "of a line cut off by the reset",
                   haltedRecently);
        return SessionEnd::newBoot;
    }
    serviceDuplex(s, passed);
    s.jlink.checkConnected();
    // the halt state every haltPoll (busyHaltPoll while passes read bytes), not every pass
    if(haltPollDue) { parts_->ops.halted = s.jlink.isHalted(); }
    noteHalt(s);
    Status const current = s.rtt->status();
    status               = current;
    // The block's id gone: a reboot (a boot ROM or the new image's init cleared it) whose S_RESET_ST this session
    // has not looked at yet - with the epoch fence only every busyHaltPoll. As a "lost connection" the next connect
    // threw the reset bit away and started without waiting for the block: the health checks of that wait (a chip
    // the DLL parked in its boot ROM) never ran. The next session waits for the block, as after a reset.
    if(current.isRunning == 0 && s.blockInfo.address != 0) {
        bool const reset = parts_->gate.resetSinceLastLook(s.jlink);
        if(reset || !controlBlockPresent(s.jlink, s.blockInfo.address)) {
            say(
              fmt::format("the RTT control block at {:#010x} is gone{}: a new RTT "
                          "session once it is back",
                          s.blockInfo.address,
                          reset ? " after a reset from outside" : ""));
            handover_.awaitBlock = true;
            if(reset) { handover_.endedEpoch = s.rtt->sessionEpoch(); }
            return SessionEnd::newBoot;
        }
    }
    if(current.isRunning == 0 || current.numUpBuffers != s.started.numUpBuffers
       || current.numDownBuffers != s.started.numDownBuffers)
    {
        throw std::runtime_error("lost connection");
    }
    warnIfQuiet(s);
    if(auto const end = checkQuietTarget(s)) { return end; }
    // no pause after a pass that found bytes: the target is writing, the next pass reads at once
    if(!morePending) { Clock::sleep_for(std::chrono::milliseconds{1}); }
    serviceProbeListRequest();
    parts_->ops.serve(s.jlink);
    return std::nullopt;
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::sayNewBoot(Session&         s,
                                                 std::string_view what,
                                                 std::string_view cut,
                                                 bool             haltedRecently) -> void {
    auto const unfinished = s.drainAll(haltedRecently).second;
    say(fmt::format("{}: a new RTT session{}", what, unfinishedText(unfinished, cut)));
    handover_.awaitBlock = true;
    handover_.endedEpoch = s.rtt->sessionEpoch();
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::unfinishedText(std::size_t      bytes,
                                                     std::string_view what) -> std::string {
    if(bytes == 0) { return {}; }
    return fmt::format(" ({} byte{} {} dropped)", bytes, bytes == 1 ? "" : "s", what);
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::pollAnnouncedReset(Session& s,
                                                         bool     haltedRecently)
  -> std::optional<SessionEnd> {
    auto const request = s.announce.pendingRequest(s.jlink);
    if(!request) { return std::nullopt; }
    Clock::sleep_for(uc_log::printer::reader::AnnouncedResets<TransportT>::drain);
    auto const unfinished = s.drainAll(haltedRecently).second;
    if(!s.announce.stillPending(s.jlink, *request)) {
        say(fmt::format(
          "the target announced a reset and did it before the printer answered: a new RTT "
          "session{}",
          unfinishedText(unfinished, "of an unfinished line")));
        handover_.awaitBlock = true;
        return SessionEnd::newBoot;
    }
    auto const pause = s.announce.takePause(*request);
    say(fmt::format("the target announced a reset: the probe stays off it for {} ms{}",
                    pause.count(),
                    unfinishedText(unfinished, "of an unfinished line")));
    s.rtt->stop();
    if(s.blockInfo.address != 0) { s.jlink.writeWord(s.blockInfo.address, 0); }
    s.announce.ack(s.jlink, *request);
    return SessionEnd::announced;
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::serviceDuplex(Session&                              s,
                                                    std::span<std::span<std::byte> const> passed)
  -> void {
    for(auto const& dc : s.channelMap.duplexChannels) {
        if(dc.upIndex && duplexBridge.sendToClient) {
            auto const data
              = *dc.upIndex < passed.size() ? passed[*dc.upIndex] : std::span<std::byte>{};
            if(!data.empty()) {
                s.lastMessage = Clock::now();   // live duplex traffic proves the link is alive
                duplexBridge.sendToClient(dc.ordinal, data);
            }
        }
        if(duplexBridge.peekFromClient && duplexBridge.consumeFromClient) {
            auto const pending = duplexBridge.peekFromClient(dc.ordinal, s.duplexOut);
            if(pending == 0) { continue; }
            auto const written
              = s.rtt->rttWrite(dc.downIndex,
                                std::span<std::byte const>{s.duplexOut}.first(pending));
            if(written != 0) { duplexBridge.consumeFromClient(dc.ordinal, written); }
        }
    }
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::noteHalt(Session& s) -> void {
    auto& ops = parts_->ops;
    if(std::exchange(ops.resumed, false)) { s.wasHalted = false; }
    bool const halted = ops.halted;
    if(halted) {
        s.lastHaltDetected = Clock::now();
        if(!s.wasHalted) {
            if(ops.haltRequested) {
                sayTool("core halted by a control-socket halt request");
            } else {
                sink_.messages.halt(uc_log::printer::reader::captureHalt(s.jlink));
            }
        }
    }
    s.wasHalted = halted;
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::warnIfQuiet(Session& s) -> void {
    auto const quietSeconds = noLogTimeoutSeconds_.load();
    if(quietSeconds == 0 || s.quietWarned
       || Clock::now() <= s.lastMessage + std::chrono::seconds{quietSeconds})
    {
        return;
    }
    s.quietWarned = true;
    say(fmt::format("no log messages for {} s, link is alive", quietSeconds));
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::checkQuietTarget(Session& s) -> std::optional<SessionEnd> {
    if(Clock::now() <= s.lastMessage + stallCheckAfter || Clock::now() <= s.nextStallCheck) {
        return std::nullopt;
    }
    s.nextStallCheck  = Clock::now() + stallCheckAfter;
    auto const action = health_.checkQuietSession(s.jlink, s.lastMessage);
    if(action == Health::Action::printerReset) {
        targetResetFlag = true;   // the printer's ordinary reset: the rings are drained first
        return SessionEnd::stopOrReplace;
    }
    if(action == Health::Action::recovered || action == Health::Action::resetLine) {
        handover_.awaitBlock = true;   // the next session waits for the new boot's block
        afterRecovery(action);
        return SessionEnd::recovered;
    }
    return std::nullopt;
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::endSession(Session&               s,
                                                 SessionEnd             end,
                                                 std::stop_token const& stoken)
  -> std::optional<std::chrono::milliseconds> {
    if(end == SessionEnd::stopOrReplace) {
        try {
            auto const before = s.drainAll(true).first;
            s.rtt->stop();
            auto const [after, unfinished] = s.drainAll(true);
            sayTool(fmt::format("the session ends: {} + {} bytes read and decoded at its end",
                                before,
                                after));
            if(unfinished != 0) {
                say(fmt::format("the session ends: {}", unfinishedLines(s, unfinished)));
            }
        } catch(std::exception const& e) {
            sayToolError(fmt::format("draining at the end of the session: {}", e.what()));
        }
    }
    if(stoken.stop_requested()) { s.announce.disarm(s.jlink); }
    if(end == SessionEnd::announced) { return s.announce.pause(); }
    return std::nullopt;
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::unfinishedLines(Session&    s,
                                                      std::size_t unfinished) -> std::string {
    std::string where;
    for(auto const& [upIndex, channel] : s.channels) {
        if(channel.buffer.empty()) { continue; }
        where += fmt::format(" [ch {}: {:02x}]",
                             upIndex,
                             fmt::join(std::span{channel.buffer}.first(
                                         std::min<std::size_t>(channel.buffer.size(), 40)),
                                       " "));
    }
    return fmt::format("{} byte{} of an unfinished line dropped{}",
                       unfinished,
                       unfinished == 1 ? "" : "s",
                       where);
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::pauseForAnnouncedReset(std::stop_token const&    stoken,
                                                             std::chrono::milliseconds pause)
  -> void {
    pausedForReset_ = true;
    auto const end  = Clock::now() + pause;
    while(!stoken.stop_requested() && Clock::now() < end) {
        parts_->ops.failAll(std::string{pausedMessage});
        Clock::sleep_for(std::chrono::milliseconds{10});
    }
    pausedForReset_ = false;
    parts_->ops.failAll(std::string{pausedMessage});
    // the next session waits for the new boot's control block (its id was cleared)
    handover_.awaitBlock = true;
    // a printer stopped during the pause does not connect again (the reader loop ends):
    // saying so would make a park in the reboot look like this printer's doing
    sayTool(stoken.stop_requested() ? "the printer stops during an announced reset: no new session"
                                    : "the announced reset is over: connecting again");
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::ramImageStart() const
  -> std::optional<uc_log::printer::RamImageStartResult> {
    if(!imageSource.ramImage) { return std::nullopt; }
    return imageSource.ramImage();
}

template<typename TransportT>
template<typename Reported>
auto BasicJLinkRttReader<TransportT>::awaitOutcome(Outcome&                     outcome,
                                                   Reported const&              reported,
                                                   Clock::time_point            end,
                                                   std::function<bool()> const& cancelled)
  -> Awaited {
    std::optional<std::uint64_t> session;
    while(Clock::now() < end && !(cancelled && cancelled())) {
        Clock::sleep_for(std::chrono::milliseconds{20});
        if(!session) {
            std::scoped_lock<std::mutex> const lock{outcome.mutex};
            if(!reported(outcome.serial)) { continue; }
            if(!outcome.error.empty()) { return Awaited{true, false, outcome.error}; }
            // a session that began while the request was on its way belongs to the image before the flash
            session = outcome.sessionsBefore;
        }
        if(sessionCount() != *session && getStatus().isRunning != 0) {
            return Awaited{true, true, {}};
        }
    }
    return Awaited{session.has_value(), false, {}};
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::setOutcome(Outcome&      o,
                                                 std::string   error,
                                                 std::uint64_t sessions) -> void {
    std::scoped_lock<std::mutex> const lock{o.mutex};
    ++o.serial;
    o.sessionsBefore = sessions;
    o.error          = std::move(error);
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::getStatus() const -> Status {
    return status;
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::linkStatus() const -> LinkStatus {
    Status const st = status;
    return LinkStatus{st.numBytesTransferred,
                      st.numBytesRead,
                      st.hostOverflowCount,
                      st.isRunning,
                      st.numUpBuffers,
                      st.numDownBuffers};
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::resetJLink() -> void {
    jlinkResetFlag = true;
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::accessTarget(std::span<TargetOp const> ops,
                                                   std::chrono::milliseconds timeout)
  -> TargetResult {
    if(pausedForReset_) { return std::unexpected{std::string{pausedMessage}}; }
    return parts_->ops.access(ops, timeout);
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::rttBytes() const -> std::uint64_t {
    return rttBytes_.load(std::memory_order_relaxed);
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::probeSerial() const -> std::uint32_t {
    return probeSerial_.load(std::memory_order_relaxed);
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::sessionCount() const -> std::uint64_t {
    return sessionCount_.load(std::memory_order_acquire);
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::isHalted() const -> bool {
    return parts_->ops.halted;
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::flashAndWait(std::chrono::milliseconds    timeout,
                                                   std::function<bool()> const& cancelled)
  -> std::expected<void,
                   std::string> {
    std::uint64_t serial{};
    {
        std::scoped_lock<std::mutex> const lock{flashOutcome_.mutex};
        serial = flashOutcome_.serial;
    }
    auto const end = Clock::now() + timeout;
    flashAttempt   = 0;
    flashFlag      = true;
    auto const a   = awaitOutcome(
      flashOutcome_,
      [serial](std::uint64_t now) { return now != serial; },
      end,
      cancelled);
    if(a.ok) { return {}; }
    if(!a.error.empty()) { return std::unexpected{a.error}; }
    // withdrawn, or it would fire whenever a target shows up
    if(!a.taken && flashFlag.exchange(false)) {
        return std::unexpected{std::string{
          Clock::now() < end ? "not flashed: cancelled (the request is withdrawn)"
                             : "not flashed: no target connected (the request is withdrawn)"}};
    }
    return std::unexpected{std::string{a.taken ? "flashed, but the log did not come back"
                                       : Clock::now() < end ? "cancelled"
                                                            : "timed out"}};
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::resetAndWait(std::chrono::milliseconds    timeout,
                                                   std::function<bool()> const& cancelled,
                                                   bool line) -> std::expected<void,
                                                                               std::string> {
    // counted before the flag is set, so the reset that takes the flag covers this number
    auto const request = ++resetRequests_;
    auto const end     = Clock::now() + timeout;
    if(line) { resetLineRequested_ = true; }   // before the flag: the reset that takes it sees it
    targetResetFlag = true;
    auto const a    = awaitOutcome(
      resetOutcome_,
      [request](std::uint64_t now) { return now >= request; },
      end,
      cancelled);
    if(a.ok) { return {}; }
    if(!a.error.empty()) { return std::unexpected{"not reset: " + a.error}; }
    // withdrawn, or it would reset the board whenever it comes back
    if(!a.taken && targetResetFlag.exchange(false)) {
        resetLineRequested_ = false;
        return std::unexpected{std::string{
          Clock::now() < end ? "not reset: cancelled (the request is withdrawn)"
                             : "not reset: no target connected (the request is withdrawn)"}};
    }
    return std::unexpected{std::string{a.taken              ? "reset, but the log did not come back"
                                       : Clock::now() < end ? "cancelled"
                                                            : "timed out"}};
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::setConnection(ConnectionSettings settings) -> void {
    {
        std::scoped_lock<std::mutex> lock{connectionMutex};
        pendingConnection = std::move(settings);
    }
    jlinkResetFlag = true;
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::refreshProbes() -> void {
    probeListRequest = true;
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::getProbes() const -> std::vector<Probe> {
    std::scoped_lock<std::mutex> lock{probesMutex};
    return probes_;
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::getProbesVersion() const -> std::uint64_t {
    return probesVersion_.load(std::memory_order_acquire);
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::resetTarget() -> void {
    targetResetFlag = true;
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::isRamImage() const -> bool {
    return static_cast<bool>(imageSource.ramImage);
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::isPausedForReset() const -> bool {
    return pausedForReset_;
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::flash() -> void {
    flashFlag = true;
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::isFlashing() const -> bool {
    return flashFlag || flashing_;
}

template<typename TransportT>
auto BasicJLinkRttReader<TransportT>::setNoLogTimeout(std::uint32_t seconds) -> void {
    noLogTimeoutSeconds_ = seconds;
}
