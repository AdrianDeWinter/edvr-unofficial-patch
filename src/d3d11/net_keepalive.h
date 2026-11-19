// Keep each HTTP pump thread's first libcurl multi handle, so loading reuses
// the connection instead of paying a TLS handshake per file. See
// net_keepalive_core.h. Installed once from the first export call, never from
// DllMain. Stands down unless this is build 334925 and the pump's two call
// sites still match that build's bytes: hooking one site without the other
// would free a handle the table still returns.
#pragma once

namespace edvr {

void netKeepaliveInstall() noexcept;

}  // namespace edvr
