#pragma once
#include "jlink/JLink.hpp"
#include "remote_fmt/catalog.hpp"
#include "remote_fmt/wire.hpp"
#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/printer/Clock.hpp"
#include "uc_log/printer/RttBlockInfo.hpp"
#include "uc_log/printer/RttChannelMap.hpp"
#include "uc_log/printer/TargetHealth.hpp"
#include "uc_log/printer/TargetPort.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uc_log::printer {
template<typename TransportT>
class RttSource;
}   // namespace uc_log::printer

namespace uc_log::printer {
class MessageLog;

namespace reader {
    // the reader's protocols with state of their own (JLinkRttReaderImpl.hpp: the flash gate, the boot count, the
    // target ops; printer/reader/)
    template<typename TransportT>
    struct Parts;
}   // namespace reader

/// Reads on the reader thread itself (ImageSource::onSessionStart / decodeGate); false = unreadable.
using DirectMemoryRead = std::function<bool(std::uint32_t, std::span<std::byte>)>;

/// Where a RAM image starts (ImageSource::ramImage), or why that is not known.
using RamImageStartResult = std::expected<JLink::RamImageStart, std::string>;

/// The probe and the target the reader connects to; host and probe change later through setConnection.
struct ReaderConnection {
    std::string   host{};     // a network J-Link; empty: USB
    std::string   device{};   // the J-Link device name
    std::uint32_t speed{};    // SWD, kHz
    std::string   probe{};    // by serial number or nickname; empty: the only one on USB
};

/// Where the reader's findings go (jlinkPrinter: the MessageLog and the merge queue; a test: its collector).
struct ReaderSink {
    MessageLog& messages;   // status / error / tool / tool error
    /// a decoded log line of the log channel `channel` (its up ring), on the decode thread. Required, as are
    /// ImageSource's block, hexFile and catalog: no default member initializer, so a designated initializer that
    /// leaves one out is refused (-Wmissing-designated-field-initializers) instead of a reader that fails at run time
    std::function<void(std::size_t, std::string_view, std::optional<remote_fmt::catalog_id>)> entry;
    /// (unread boots, count from, count to), see printer/BootLedger.hpp - also said as a status error
    std::function<void(std::uint32_t, std::uint32_t, std::uint32_t)> unreadBoots{};
    /// the count of boots that logged starts again (the printer's own download), see restartBootCount
    std::function<void()> bootCountRestart{};
};

/// What the reader asks about the image the target should run, at every session start (a rebuild between sessions
/// counts): from the map, the hex file and the catalog. Called on the reader thread.
struct ImageSource {
    std::function<RttBlockInfo()>
      block;   // the RTT control block: the map's address, or a RAM search
    std::function<std::string()> hexFile;   // what flash() writes
    std::function<std::unordered_map<std::uint16_t, std::string>()>
      catalog;   // the session's strings
    /// what the image says about its frames (jlinkPrinter: "FrameCheck" in the catalog); empty: either kind is taken
    std::function<remote_fmt::FrameCheck()> frameCheck{};
    /// the announced-reset block's address (Kvasir_SDK Runtime/AnnouncedReset.hpp, from the map); empty or nullopt:
    /// none. The printer arms it; when the firmware announces a reset of its own the session is closed and the probe
    /// stays off the chip for announced_reset::printerPause.
    std::function<std::optional<std::uint32_t>()> announcedReset{};
    /// the address of the image's RTT epoch (detail/RttEpoch.hpp, from the map): fences every ring access of the
    /// printer's own RTT reading against a boot re-initialising the block. Empty or nullopt: none
    std::function<std::optional<std::uint32_t>()> rttEpoch{};
    /// the address of the image's count of boots that logged (the word after the epoch), also asked before every own
    /// flash or reset: the unread boots are counted by it (printer/BootLedger.hpp). Empty or nullopt: no count
    std::function<std::optional<std::uint32_t>()> loggedBoots{};
    /// set: the image lives in RAM (a Kvasir RAM_ONLY target) - a reset would boot flash, so flash and reset load it
    /// and start it at its vector table (JLink::flashRamImage); asked anew before each (a rebuild may move it)
    std::function<RamImageStartResult()> ramImage{};
    /// asked after onSessionStart and then every decodeGatePoll while it says no: whether the log may be decoded with
    /// the files at hand (jlinkPrinter: the firmware check did not say "different"); empty: always
    std::function<bool(DirectMemoryRead const&)> decodeGate{};
    /// at every session start, before the first log line is read (jlinkPrinter: the firmware check)
    std::function<void(DirectMemoryRead const&)> onSessionStart{};
};

/// How the reader treats the chip (the chip package's printer options), fixed for the reader's life.
struct ReaderOptions {
    std::vector<JLink::MemoryWrite> preResetCommands{};   // written before every reset and download
    /// a stall the chip package can tell and undo (printer/TargetHealth.hpp); nullopt: none
    std::optional<printer::StallSignature> stall{};
    int                                    stallFirstStep{
      1};   // a stall starts the recovery ladder there (a test switch: UC_LOG_STALL_FIRST_STEP)
    std::optional<printer::ParkSignature>
      park{};   // printer/TargetHealth.hpp; nullopt: a chip that does not park
    /// the register words that hold while the target's flash is mapped (its boot stage has set XIP up): the firmware
    /// check reads the flash only then (flashRead). Empty: always
    std::vector<printer::StallSignature::Condition> flashReady{};
    /// the chip's flash window (XIP and its interface's registers): a control-socket read there goes through
    /// flashRead's gate. nullopt: none
    std::optional<std::pair<std::uint32_t, std::uint32_t>> flashWindow{};
    bool                                                   directRtt{
      true};   // false: RTT through the J-Link DLL even where the printer could read it itself
    /// on the reader thread before every connect (jlinkPrinter: SEGGER's daily EDU terms accepted for today - a
    /// session can run past midnight)
    std::function<void()> beforeConnect{};
};
}   // namespace uc_log::printer

// Templated on the transport so the reader loop (framing, liveness, overflow reporting,
// reconnect policy) is testable with an in-memory fake instead of the JLink DLL.
template<typename TransportT = JLink>
struct BasicJLinkRttReader final : uc_log::printer::TargetPort {
    /// Signs of a stuck target the chip package names (printer/TargetHealth.hpp): ReaderOptions::park / stall.
    using ParkSignature  = uc_log::printer::ParkSignature;
    using StallSignature = uc_log::printer::StallSignature;

private:
    using Clock  = uc_log::printer::Clock;
    using Status = typename TransportT::Status;

public:
    using Probe = uc_log::printer::TargetPort::Probe;

    /// What other threads ask of the target (printer/TargetOp.hpp), and the answer: bytes per op, or why not.
    using TargetOp     = uc_log::printer::TargetOp;
    using TargetResult = uc_log::printer::TargetPort::Result;

    using ConnectionSettings = uc_log::printer::TargetPort::ConnectionSettings;

private:
    static constexpr std::size_t rttBufferChunkSize = 32768;
    static constexpr std::size_t duplexChunkSize    = 4096;
    static constexpr auto        haltPoll           = std::chrono::milliseconds{10};
    // J-Link reset type 2: through the probe's nRESET pin (the reset line; the stall ladder's step 3 uses it too)
    static constexpr std::uint8_t resetTypeLine = 2;
    // The halt/S_RESET_ST poll interval while passes read bytes: each poll is a probe round trip, and a target that
    // halts or resets stops filling the rings, so the next empty pass brings back haltPoll. Kept a pass below the
    // frame timeout: a core halted mid-frame beside one that floods on must be seen as halted before its frame is
    // resynced away (RttChannel::drain resyncs only while no halt was seen).
    // (JLinkRttReaderImpl.hpp: below RttChannel::frameTimeout, a static_assert)
    static constexpr auto busyHaltPoll          = std::chrono::milliseconds{50};
    using Health                                = uc_log::printer::TargetHealth<TransportT>;
    static constexpr auto     stallCheckAfter   = Health::stallCheckAfter;
    static constexpr auto     haltGracePeriod   = std::chrono::seconds{60};
    static constexpr unsigned flashAttempts     = 3;
    static constexpr auto     retryBootTime     = std::chrono::milliseconds{500};   // prepareRetry
    static constexpr auto     controlBlockPoll  = std::chrono::milliseconds{20};
    static constexpr auto     healthCheckPeriod = std::chrono::milliseconds{1000};
    static constexpr auto     decodeGatePoll    = std::chrono::milliseconds{500};
    static constexpr auto     targetSilentLimit = std::chrono::milliseconds{3000};
    static constexpr int      silentResetAfter  = 3;   // silent connections before the reset line
    static constexpr auto     recoveryOffProbe  = std::chrono::milliseconds{1500};

    // the control block's id is at its address: the image has started
    static bool controlBlockPresent(TransportT&   jlink,
                                    std::uint32_t address);

    // The probe got no answer from the target for targetSilentLimit: the DLL's connection is closed and opened again (a
    // watchdog reset of the RP2040 by the stall recovery left every DLL call failing -1 for 6.5 minutes, on one
    // connection, while the health checks took each failure for "no sign"). run() escalates when it keeps happening.
    struct TargetSilent : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    // While a wait polls the target: whether it still answers - CPUID (0xE000ED00), which a read leaves as it is (a
    // DHCSR read would clear S_RESET_ST, the sign of a reset from outside); silent for targetSilentLimit -> TargetSilent.
    void checkTargetAnswers(TransportT&        jlink,
                            Clock::time_point& lastAnswer);

    // Every connection first sees the target answer, within targetSilentLimit - else TargetSilent (run() escalates).
    // Without it a silent target made the session start's reads fail into the J-Link DLL's own RTT search, which then
    // searched a target nobody could read for minutes.
    void awaitTargetAnswers(TransportT&            jlink,
                            std::stop_token const& stoken);

    // After a reset from outside, the next session waits for the new boot's control block: the target may run its boot
    // ROM for a while (RP2350 BOOTSEL zeroes the block), and a session begun then reports its flash as a firmware
    // mismatch and fails startRtt after seconds of RAM scans. A block alone is not the new boot's: RAM keeps the old one
    // while the boot ROM runs, so with an epoch it waits for another epoch than the ended boot's (the new boot moved it
    // before it initialised its block) - and the firmware check after it reads a flash the boot stage has mapped.
    void awaitControlBlockAfterReset(TransportT&            jlink,
                                     std::stop_token const& stoken);

    // The DLL is only ever driven from this thread, so the probe list the GUI asked for is
    // produced here too. A failed listing is reported, never a reason to reconnect.
    void serviceProbeListRequest();

    // The session's RTT: the printer's own where the map gives the control block's address (and the DLL's is not
    // asked for), the J-Link DLL's otherwise (a block found by the DLL's RAM search).
    std::unique_ptr<uc_log::printer::RttSource<TransportT>>
    openRttSource(TransportT&                          jlink,
                  uc_log::printer::RttBlockInfo const& blockInfo);

    std::optional<std::uint32_t> rttEpochAddress() const;

    remote_fmt::FrameCheck frameCheckOfImage() const;

    bool decodeAllowed(TransportT& jlink);

    // The flash gate's look at the target's health before it waits for the flash (reader::FlashGate::HealthLook): a
    // parked chip is reset, a stalled or locked-up one recovered; true: so it was, the read gives up.
    bool healthBeforeFlashWait(TransportT& jlink);

    // One connection a turn: connect, serve a pending flash / reset (a new connection follows it), wait for the
    // control block where a reset left the target, run one session. A failure reconnects after a second; an announced
    // reset keeps the probe off the chip for the pause the target asked for.
    void run(std::stop_token const& stoken);

    // After a recovery that reboots the target: the next connection waits recoveryOffProbe first (run()); the reset line
    // is the next connection's reset.
    void afterRecovery(typename Health::Action action);

    // A target that stays silent across connections: connect anew (each time), then the reset line once, then ask for a
    // power cycle - and keep trying. A session that reads the log again starts over (silentConnections_ = 0).
    void escalateSilentTarget(std::string_view what);

    // a host / probe set meanwhile (setConnection) applies from this connection on; ReaderOptions::beforeConnect runs
    void takePendingConnection();

    // A pending flash or reset, served; true: the target was replaced or reset, a new connection follows. A request
    // is taken while it runs: it can only be withdrawn before or between attempts.
    bool serviceFlashAndReset(TransportT& jlink);

    bool flashTarget(TransportT& jlink);

    // Before the next attempt at a failed download: the package's stall recovery, then a reset with the core let run
    // for retryBootTime: a flash interface a boot ROM left half set up can fail every download ("RAMCode-sided
    // Prepare() Error") until the boot ROM has booted once. Costs retryBootTime on a download that failed anyway.
    void prepareRetry(TransportT&      jlink,
                      std::string_view why);

    // The printer's own reset (a RAM image is loaded again: a reset boots flash); true: reset.
    bool resetTargetNow(TransportT& jlink);

    // Why a session ended, and what follows it.
    enum class SessionEnd {
        stopOrReplace,   // the printer stops, or resets / flashes the target: the rings are drained
        announced,       // the target announced a reset: drained, acked, the probe stays away
        newBoot,   // a boot the printer did not ask for (outside reset, re-init, a late announced reset)
        recovered,   // a health recovery: the next session waits for the new boot's block
    };

    // What lives for one session (JLinkRttReaderImpl.hpp): the RTT source, the log channels and their decoder, the
    // read chunks, the announced-reset block, the loop's clocks. Built when the session opens, gone when it ends.
    struct LogChannel;
    class Session;

    // One session: the catalog, the session-start callback (the firmware check, before any RTT read: the target keeps
    // its lines in its rings meanwhile), the decode gate, the announced-reset block armed, then the read loop until
    // something ends it. The announced pause when the target announced a reset.
    std::optional<std::chrono::milliseconds> runSession(TransportT&            jlink,
                                                        std::stop_token const& stoken);

    void runSessionStart(TransportT& jlink);

    // The decode gate: no RTT read while the strings would be the wrong ones (the target runs another image than
    // the files describe). Nothing is read, so nothing is lost: the target keeps its lines in its rings (and counts
    // what does not fit). Opens when the files change to match (a rebuild); a flash or reset starts a new session
    // anyway. Opening ends this session so the next one loads the new catalog and map.
    void holdAtGate(TransportT&            jlink,
                    std::stop_token const& stoken);

    // One pass of the read loop; a reason when the session ends.
    std::optional<SessionEnd> readPass(Session&               s,
                                       std::stop_token const& stoken);

    // A boot the printer did not ask for ends the session: what the rings hold is taken first, the next session waits
    // for the new boot's block.
    void sayNewBoot(Session&         s,
                    std::string_view what,
                    std::string_view cut,
                    bool             haltedRecently);

    static std::string unfinishedText(std::size_t      bytes,
                                      std::string_view what);

    // The firmware's announced reset (Kvasir_SDK Runtime/AnnouncedReset.hpp, reader::AnnouncedResets): a pending
    // request is answered - drain the last lines, stop RTT, clear the control block's id (a reboot into a boot ROM keeps
    // RAM), ack as the last access; the probe then stays away.
    std::optional<SessionEnd> pollAnnouncedReset(Session& s,
                                                 bool     haltedRecently);

    void serviceDuplex(Session&                              s,
                       std::span<std::span<std::byte> const> passed);

    // A halt a client asked for is no crash: said as a tool message, so a wait for a log line beside it (kvasir_bench
    // wait-for ends on "core halted") does not fail. Any other halt is captured (the fault handler's bkpt).
    // A go from a request ran the core even when it halts again before the next pass (a raised panic's bkpt is
    // microseconds away): that halt is a new one.
    void noteHalt(Session& s);

    // a quiet target is not a dead link (checkConnected and the status say that): only a warning
    void warnIfQuiet(Session& s);

    // A target quiet for stallCheckAfter: in lockup, or stalled in a way its chip package can tell (and undo)?
    std::optional<SessionEnd> checkQuietTarget(Session& s);

    // The end of a session. Stopping, or replacing the target by the printer's own flash or reset: what is in the
    // rings is read and decoded now - with the DLL's RTT first what the DLL holds, then its RTT stops, then
    // what it read meanwhile (what it took and had not handed over is lost at the stop: the mode's weakness). Every
    // end: a printer that goes disarms the announced-reset block (a later reset of the firmware must not wait for it).
    std::optional<std::chrono::milliseconds> endSession(Session&               s,
                                                        SessionEnd             end,
                                                        std::stop_token const& stoken);

    // "N bytes of an unfinished line dropped" with the first bytes of each channel's (for finding where a record broke)
    static std::string unfinishedLines(Session&    s,
                                       std::size_t unfinished);

    // The session is closed (the transport is gone): no DLL call until the pause is over, and
    // memory accesses asked meanwhile fail at once instead of waiting for it.
    void pauseForAnnouncedReset(std::stop_token const&    stoken,
                                std::chrono::milliseconds pause);

    static constexpr std::string_view pausedMessage{
      "target paused for an announced reset: the probe stays off it"};

    std::string   host;   // connectionMutex's pendingConnection replaces it on the reader thread
    std::string   device;
    std::uint32_t speed;
    std::string   probe;   // as host

    uc_log::printer::ReaderSink          sink_;
    uc_log::printer::ImageSource const   imageSource;
    uc_log::printer::ReaderOptions const readerOptions;
    uc_log::printer::DuplexBridge        duplexBridge;

    std::atomic<Status>               status;
    std::atomic<std::uint32_t>        noLogTimeoutSeconds_{15};
    std::atomic<bool>                 targetResetFlag;
    std::atomic<bool>                 jlinkResetFlag;
    std::atomic<bool>                 flashFlag;
    std::mutex                        connectionMutex;
    std::optional<ConnectionSettings> pendingConnection;   // connectionMutex

    // nullopt: an ordinary image; else where the RAM image starts, or why that is not known
    std::optional<uc_log::printer::RamImageStartResult> ramImageStart() const;

    void say(std::string_view text) const;
    void sayError(std::string_view text) const;
    void sayTool(std::string_view text) const;
    void sayToolError(std::string_view text) const;

    // empty error = success
    struct Outcome {
        std::mutex    mutex;
        std::uint64_t serial{};
        std::uint64_t sessionsBefore{};
        std::string   error;
    };

    struct Awaited {
        bool        taken{};   // the reader acted on the request
        bool        ok{};      // and a session after it logs
        std::string error;     // the reader's reason it failed
    };

    // flashAndWait's and resetAndWait's wait: until `outcome` reports the request (`reported(serial)`), then until a
    // session begun after it runs (isRunning too: the firmware check sits between session start and log)
    template<typename Reported>
    Awaited awaitOutcome(Outcome&                     outcome,
                         Reported const&              reported,
                         Clock::time_point            end,
                         std::function<bool()> const& cancelled);

    static void setOutcome(Outcome&      o,
                           std::string   error,
                           std::uint64_t sessions);

    Outcome                    flashOutcome_;
    Outcome                    resetOutcome_;   // serial: requests covered
    std::atomic<std::uint64_t> resetRequests_{0};
    std::atomic<unsigned>      flashAttempt{};
    std::atomic<bool>          flashing_{false};
    std::atomic<std::uint64_t> sessionCount_{0};
    std::atomic<std::uint32_t> probeSerial_{0};
    std::atomic<bool>          pausedForReset_{false};

    // What one session leaves to the next (reader thread): whether to wait for the new boot's control block, and
    // the unfinished bytes of each log channel (by up ring) of a session that died by an exception, with its RTT
    // epoch - the next session continues them if the target has not booted since.
    struct Handover {
        bool awaitBlock{};
        // the RTT epoch of the boot the last session read: after a reset the next one waits for a block of another
        // epoch - the old boot's block stays in RAM while the boot ROM runs
        std::optional<std::uint32_t>                    endedEpoch;
        std::map<std::uint32_t, std::vector<std::byte>> carry;
        std::optional<std::uint32_t>                    carryEpoch;
    } handover_;

    // park, lockup and stall checks; declared before `thread`
    Health                     health_;
    std::atomic<std::uint64_t> rttBytes_{0};
    int                        silentConnections_{};   // reader thread
    bool                       resetPinOnce_{};   // reader thread: the next reset by the reset line
    std::atomic<bool>
         resetLineRequested_{};   // resetAndWait(line): the next resetTargetNow by the line
    bool offProbeOnce_{};         // reader thread: pause before the next connect
    std::atomic<bool>          probeListRequest{false};
    mutable std::mutex         probesMutex;
    std::vector<Probe>         probes_;
    std::atomic<std::uint64_t> probesVersion_{0};
    // the flash gate, the boot count, the target ops (and the halt state): after health_ and the configuration they
    // are given, before `thread`
    std::unique_ptr<uc_log::printer::reader::Parts<TransportT>> parts_;
    Clock::Thread                                               thread;

public:
    /// Configured once: the reader thread starts in the constructor (a setter after it could race the first session).
    BasicJLinkRttReader(uc_log::printer::ReaderConnection connection,
                        uc_log::printer::ReaderSink       sink,
                        uc_log::printer::ImageSource      image,
                        uc_log::printer::ReaderOptions    options      = {},
                        uc_log::printer::DuplexBridge     duplexBridge = {});

    ~BasicJLinkRttReader() override;

    BasicJLinkRttReader(BasicJLinkRttReader const&)            = delete;
    BasicJLinkRttReader& operator=(BasicJLinkRttReader const&) = delete;

    Status getStatus() const;

    LinkStatus linkStatus() const override;

    void resetJLink() override;

    /// Runs `ops` on the reader thread, the core running. From any thread; blocks until done or `timeout`.
    /// TargetPort::defaultTimeout is long enough for a busy probe: on an RP2040 at 2 MHz SWD under a log flood one pass
    /// of the reader took over 250 ms (the DLL's own RTT polling holds the probe), and pokes failed.
    TargetResult accessTarget(std::span<TargetOp const> ops,
                              std::chrono::milliseconds timeout) override;

    /// bytes of log read from the target's rings since the printer started
    std::uint64_t rttBytes() const;

    /// the serial number of the probe of the last session (0 before the first)
    std::uint32_t probeSerial() const;

    std::uint64_t sessionCount() const override;

    bool isHalted() const override;

    /// Returns once the log runs again. From any thread but the reader's. `cancelled`: the caller
    /// gives up early (printer stopping, client gone).
    std::expected<void,
                  std::string>
    flashAndWait(std::chrono::milliseconds    timeout,
                 std::function<bool()> const& cancelled = {});

    std::expected<void,
                  std::string>
    resetAndWait(std::chrono::milliseconds    timeout,
                 std::function<bool()> const& cancelled = {},
                 bool                         line      = false);

    // Reconnect with a different host / probe.
    void setConnection(ConnectionSettings settings) override;

    // Ask the reader thread to list the probes; getProbes() has the result once
    // getProbesVersion() changed.
    void refreshProbes() override;

    std::vector<Probe> getProbes() const override;

    std::uint64_t getProbesVersion() const override;

    void resetTarget() override;

    bool isRamImage() const;

    /// True while the probe stays off the chip for an announced reset.
    bool isPausedForReset() const;

    void flash() override;

    bool isFlashing() const override;

    void setNoLogTimeout(std::uint32_t seconds) override;
};

using JLinkRttReader = BasicJLinkRttReader<>;

// instantiated once, in printer/ReaderJLink.cpp (the printer's core library); the members are in JLinkRttReaderImpl.hpp
extern template struct BasicJLinkRttReader<JLink>;
