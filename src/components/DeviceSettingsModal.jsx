"use client"
import { useState, useEffect } from "react"

const TIMEZONES = Intl.supportedValuesOf("timeZone")

export default function DeviceSettingsModal({ device, googleId, backendToken, onClose, onSaved }) {
  const [hourFormat, setHourFormat] = useState(device.hour_format ?? 24)
  const [timezone, setTimezone] = useState(device.timezone ?? "UTC")
  const [saving, setSaving] = useState(false)
  const [saved, setSaved] = useState(false)
  const authHeader = backendToken ? { Authorization: `Bearer ${backendToken}` } : {}

  useEffect(() => {
    function onKey(e) { if (e.key === "Escape") onClose() }
    window.addEventListener("keydown", onKey)
    return () => window.removeEventListener("keydown", onKey)
  }, [onClose])

  async function handleSave() {
    setSaving(true)
    try {
      const res = await fetch(
        `${process.env.NEXT_PUBLIC_BACKEND_URL}/api/devices/${device.id}/settings?google_id=${googleId}`,
        {
          method: "PATCH",
          headers: { "Content-Type": "application/json", ...authHeader },
          body: JSON.stringify({ hour_format: hourFormat, timezone }),
        }
      )
      if (res.ok) {
        setSaved(true)
        onSaved({ ...device, hour_format: hourFormat, timezone })
        setTimeout(onClose, 1000)
      }
    } finally {
      setSaving(false)
    }
  }

  return (
    <div style={styles.overlay} onClick={e => e.target === e.currentTarget && onClose()}>
      <div style={styles.modal}>
        <div style={styles.header}>
          <div>
            <div style={styles.title}>Device Settings</div>
            <div style={styles.sub}>ID — {String(device.id).padStart(3, "0")}</div>
          </div>
          <button style={styles.closeBtn} onClick={onClose}>✕</button>
        </div>

        <div style={styles.field}>
          <div style={styles.label}>Clock Format</div>
          <div style={styles.toggleGroup}>
            {[12, 24].map(h => (
              <button
                key={h}
                style={{ ...styles.toggleBtn, ...(hourFormat === h ? styles.toggleActive : {}) }}
                onClick={() => setHourFormat(h)}
              >
                {h}h
              </button>
            ))}
          </div>
        </div>

        <div style={styles.field}>
          <div style={styles.label}>Timezone</div>
          <select
            style={styles.select}
            value={timezone}
            onChange={e => setTimezone(e.target.value)}
          >
            {TIMEZONES.map(tz => (
              <option key={tz} value={tz}>{tz}</option>
            ))}
          </select>
        </div>

        <div style={styles.footer}>
          <button style={styles.cancelBtn} onClick={onClose}>Cancel</button>
          <button
            style={{ ...styles.saveBtn, opacity: saving ? 0.6 : 1 }}
            onClick={handleSave}
            disabled={saving}
          >
            {saved ? "✓ Saved" : saving ? "Saving..." : "Save Changes"}
          </button>
        </div>
      </div>
      <style>{`
        @keyframes modalIn { from{opacity:0;transform:translateY(12px)} to{opacity:1;transform:translateY(0)} }
      `}</style>
    </div>
  )
}

const styles = {
  overlay: {
    position: "fixed",
    inset: 0,
    background: "rgba(0,0,0,0.6)",
    display: "flex",
    alignItems: "center",
    justifyContent: "center",
    zIndex: 100,
    padding: "24px",
    backdropFilter: "blur(4px)",
  },
  modal: {
    background: "var(--surface)",
    border: "1px solid var(--border)",
    borderRadius: "16px",
    padding: "28px",
    width: "100%",
    maxWidth: "420px",
    animation: "modalIn 0.2s ease both",
  },
  header: {
    display: "flex",
    alignItems: "flex-start",
    justifyContent: "space-between",
    marginBottom: "28px",
  },
  title: {
    fontSize: "17px",
    fontWeight: "700",
    color: "var(--text)",
    marginBottom: "4px",
  },
  sub: {
    fontFamily: "var(--mono)",
    fontSize: "10px",
    color: "var(--text-muted)",
    letterSpacing: "0.08em",
  },
  closeBtn: {
    background: "none",
    border: "none",
    color: "var(--text-muted)",
    fontSize: "16px",
    cursor: "pointer",
    padding: "4px",
    lineHeight: 1,
  },
  field: { marginBottom: "24px" },
  label: {
    fontFamily: "var(--mono)",
    fontSize: "10px",
    letterSpacing: "0.1em",
    textTransform: "uppercase",
    color: "var(--text-muted)",
    marginBottom: "10px",
  },
  toggleGroup: { display: "flex", gap: "8px" },
  toggleBtn: {
    padding: "8px 20px",
    borderRadius: "8px",
    fontFamily: "var(--mono)",
    fontSize: "13px",
    fontWeight: "500",
    cursor: "pointer",
    border: "1px solid var(--border)",
    background: "transparent",
    color: "var(--text-sub)",
    transition: "all 0.12s",
  },
  toggleActive: {
    background: "var(--accent-dim)",
    borderColor: "var(--accent)",
    color: "var(--accent)",
  },
  select: {
    width: "100%",
    background: "var(--surface2)",
    border: "1px solid var(--border)",
    borderRadius: "8px",
    color: "var(--text)",
    fontFamily: "var(--mono)",
    fontSize: "12px",
    padding: "10px 14px",
    outline: "none",
    colorScheme: "dark",
  },
  footer: {
    display: "flex",
    justifyContent: "flex-end",
    gap: "10px",
    marginTop: "8px",
  },
  cancelBtn: {
    padding: "9px 18px",
    borderRadius: "8px",
    border: "1px solid var(--border)",
    background: "transparent",
    color: "var(--text-sub)",
    fontFamily: "var(--sans)",
    fontSize: "13px",
    fontWeight: "600",
    cursor: "pointer",
  },
  saveBtn: {
    padding: "9px 20px",
    borderRadius: "8px",
    border: "none",
    background: "var(--accent)",
    color: "#111",
    fontFamily: "var(--sans)",
    fontSize: "13px",
    fontWeight: "700",
    cursor: "pointer",
    transition: "opacity 0.15s",
  },
}