#pragma once

namespace rq {

// Sends Qt warnings and errors to %LOCALAPPDATA%\ResoluteQuant\native.log. The log holds job names
// and error text only: never write credentials to it.
void installFileLogging();

}  // namespace rq
