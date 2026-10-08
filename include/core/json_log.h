#pragma once

namespace houston_kvm {

// --log-format=json: from here on, every line written to std::cout or
// std::cerr comes out as one JSON object per line, for a log shipper:
//
//   {"ts":"2026-10-01T19:17:14.481Z","level":"warning","component":"V4L2","msg":"..."}
//
// stdout lines are "info" and stderr lines "warning". `component` is the
// "Name: " prefix most messages start with, when there is one. Lines are
// assembled per thread, so two threads writing at once never interleave
// within a line. Call once, before any other thread starts.
void enableJsonLogs();

} // namespace houston_kvm
