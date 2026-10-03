#pragma once

// Public half of the ECDSA P-256 key that signs firmware releases.
// Every downloaded update must carry a valid signature from the matching
// private key, which lives only in the FIRMWARE_SIGNING_KEY GitHub secret.
// Changing this key means re-flashing every device by cable.
static const char FIRMWARE_SIGNING_PUBLIC_KEY[] = R"(-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEsb2pjm7WIMjc03F3lkI19PAyrLv9
15n2LxDGbl9j4MIULHOsL6IEpODrTyGjggi/qMMMuLqd+PdDY5YYNBiK5w==
-----END PUBLIC KEY-----
)";
