// The client reports what TLS actually negotiated - not what was asked for - in its first line:
//   "Secure connection: TLSv1.3, cipher TLS_AES_256_GCM_SHA384, group X25519MLKEM768"
// A pin or CA mismatch never gets that far, so this line is the connection's proof of posture.

export interface Posture {
  encrypted: boolean;
  protocol?: string;
  cipher?: string;
  group?: string;
  postQuantum: boolean;
}

export const PLAINTEXT: Posture = { encrypted: false, postQuantum: false };

export function parsePosture(message: string): Posture | null {
  const prefix = "Secure connection:";
  if (!message.startsWith(prefix)) return null;
  const parts = message.slice(prefix.length).split(",").map((p) => p.trim());
  const posture: Posture = { encrypted: true, postQuantum: false, protocol: parts[0] || undefined };
  for (const part of parts.slice(1)) {
    const [key, ...rest] = part.split(/\s+/);
    const value = rest.join(" ");
    if (key === "cipher") posture.cipher = value;
    if (key === "group") posture.group = value;
  }
  // Hybrid groups pair a classical exchange with ML-KEM (X25519MLKEM768, SecP256r1MLKEM768, ...)
  posture.postQuantum = /MLKEM/i.test(posture.group ?? "");
  return posture;
}
