// The printer's reader on the real J-Link, instantiated once for every program of the printer (JLinkRttReader.hpp
// declares it extern). The host tests instantiate it on their FakeTransport themselves.
#include "uc_log/printer/JLinkRttReaderImpl.hpp"

template struct BasicJLinkRttReader<JLink>;
