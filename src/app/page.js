"use client"

import { useEffect, useState, useCallback } from "react"
import { useSession, signOut } from "next-auth/react"
import { useRouter } from "next/navigation"
import DeviceSettingsModal from "@/components/DeviceSettingsModal"

const API = process.env.NEXT_PUBLIC_BACKEND_URL
const DAYS = ["Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"]

export default function Home() {
  const { data: session, status } = useSession()
  const router = useRouter()

  const [devices, setDevices] = useState([])
  const [alarms, setAlarms] = useState([])
  const [deviceId, setDeviceId] = useState(null)
  const [settingsDevice, setSettingsDevice] = useState(null)

  const [time, setTime] = useState("07:00")
  const [recurring, setRecurring] = useState(false)
  const [recurType, setRecurType] = useState("daily")
  const [recurDays, setRecurDays] = useState([])

  const [toast, setToast] = useState("")
  const [toastVisible, setToastVisible] = useState(false)
  

  const backendToken = session?.backendToken
  const authHeader = backendToken ? { Authorization: `Bearer ${backendToken}` } : {}

  function showToast(msg) {
    setToast(msg); setToastVisible(true)
    setTimeout(() => setToastVisible(false), 2200)
  }

  function toggleDay(d) {
    setRecurDays(prev => prev.includes(d) ? prev.filter(x => x !== d) : [...prev, d])
  }

  const googleId = session?.user?.googleId

  const loadDevices = useCallback(async () => {
    if (!googleId) return
    try {
      const data = await fetch(`${API}/api/devices`, {
        headers: { Authorization: `Bearer ${session?.backendToken}` },
      }).then(r => r.json())
      const list = Array.isArray(data) ? data : []
      setDevices(list)
      if (!deviceId && list.length > 0) setDeviceId(list[0].id)
    } catch { setDevices([]) }
  }, [googleId, deviceId])

  const loadAlarms = useCallback(async () => {
    if (!deviceId || !googleId) return
    try {
      const data = await fetch(`${API}/api/alarms/${deviceId}`, {
        headers: { Authorization: `Bearer ${session?.backendToken}` },
      }).then(r => r.json())
      setAlarms(Array.isArray(data) ? data : [])
    } catch { setAlarms([]) }
  }, [deviceId, googleId])

  useEffect(() => {
    if (status === "unauthenticated") router.push("/login")
  }, [status, router])

  useEffect(() => {
    if (status !== "authenticated") return
    loadDevices()
    const iv = setInterval(loadDevices, 5000)
    return () => clearInterval(iv)
  }, [status, loadDevices])

  useEffect(() => { loadAlarms() }, [loadAlarms])

  async function buzzer(state) {
    await fetch(`${API}/api/buzzer`, {
      method: "POST",
      headers: { "Content-Type": "application/json", ...authHeader },
      body: JSON.stringify({ device_id: deviceId, state }),
    })
    showToast(state ? "Buzzer activated" : "Buzzer deactivated")
  }

  async function createAlarm() {
    await fetch(`${API}/api/alarms`, {
      method: "POST",
      headers: { "Content-Type": "application/json", ...authHeader },
      body: JSON.stringify({
        device_id: deviceId,
        time,
        recurring,
        recur_type: recurring ? recurType : "",
        recur_days: recurring && recurType === "weekly" ? recurDays : [],
      }),
    })
    await loadAlarms()
    showToast(`Alarm set — ${time} (${recurring ? recurType : "once"})`)
  }

  async function deleteAlarm(id) {
    await fetch(`${API}/api/alarms/${id}`, {
      method: "DELETE",
      headers: { ...authHeader },
    })
    await loadAlarms()
    showToast("Alarm deleted")
  }

  const selectedDevice = devices.find(d => d.id === deviceId)
  const onlineCount = devices.filter(d => d.online).length

  function recurLabel(alarm) {
    if (!alarm.recurring) return "once"
    if (alarm.recur_type === "daily") return "daily"
    if (alarm.recur_type === "weekly") {
      const names = (alarm.recur_days || []).map(d => DAYS[d]).join(" ")
      return names || "weekly"
    }
    return "recurring"
  }

  if (status === "loading" || !session) return null

  return (
    <>
      <style>{pageStyles}</style>
      <div className="dashboard">

        <div className="header">
          <div>
            <div className="header-title">Cloud<span>Clock</span></div>
            <div className="header-sub">IoT Alarm Management</div>
          </div>
          <div style={{ display: "flex", alignItems: "center", gap: "12px" }}>
            <div className="header-badge">{onlineCount}/{devices.length} online</div>
            <button className="pair-btn" onClick={() => router.push("/pair")}>+ Pair Device</button>
            <div className="user-chip" onClick={() => signOut({ callbackUrl: "/login" })}>
              {session.user.image
                ? <img src={session.user.image} alt="" style={{ width: 22, height: 22, borderRadius: "50%" }} />
                : <span style={{ width: 22, height: 22, borderRadius: "50%", background: "var(--border)", display: "inline-block" }} />
              }
              <span>{session.user.name?.split(" ")[0]}</span>
              <span style={{ opacity: 0.4, fontSize: 10 }}>sign out</span>
            </div>
          </div>
        </div>

        <div className="section">
          <div className="section-header">
            <span className="section-label">Devices</span>
            <div className="section-line" />
          </div>
          {devices.length === 0 ? (
            <div className="empty-state">
              No devices paired yet —{" "}
              <span style={{ color: "var(--accent)", cursor: "pointer" }} onClick={() => router.push("/pair")}>
                pair your first clock
              </span>
            </div>
          ) : (
            <div className="devices-grid">
              {devices.map(d => (
                <div key={d.id} className={`device-card${deviceId === d.id ? " selected" : ""}`} onClick={() => setDeviceId(d.id)}>
                  <div className="device-card-top">
                    <span className="device-id">ID — {String(d.id).padStart(3, "0")}</span>
                    <div className={`status-pill ${d.online ? "online" : "offline"}`}>
                      <span className="status-dot" />
                      {d.online ? "Online" : "Offline"}
                    </div>
                  </div>
                  <div className="device-name">{d.name || `Device ${d.id}`}</div>
                  <div className="device-seen">{d.timezone ?? "UTC"} · {d.hour_format ?? 24}h</div>
                  <div className="device-actions">
                    <button className="settings-btn" onClick={e => { e.stopPropagation(); setSettingsDevice(d) }}>
                      ⚙ Settings
                    </button>
                    {deviceId === d.id && (
                      <span style={{ fontFamily: "var(--mono)", fontSize: 10, color: "var(--accent)" }}>✓ Active</span>
                    )}
                  </div>
                </div>
              ))}
            </div>
          )}
        </div>

        {deviceId && (
          <>
            <div className="section">
              <div className="section-header">
                <span className="section-label">Buzzer</span>
                <div className="section-line" />
              </div>
              <div className="panel">
                <div className="panel-row">
                  <div>
                    <div className="buzzer-title">Manual Trigger</div>
                    <div className="buzzer-sub">Device {String(deviceId).padStart(3, "0")} — {selectedDevice?.online ? "reachable" : "offline"}</div>
                  </div>
                  <div className="btn-row">
                    <button className="btn btn-on" onClick={() => buzzer(true)}>🔔 Activate</button>
                    <button className="btn btn-off" onClick={() => buzzer(false)}>🔕 Deactivate</button>
                  </div>
                </div>
              </div>
            </div>

            <div className="section">
              <div className="section-header">
                <span className="section-label">Schedule Alarm</span>
                <div className="section-line" />
              </div>
              <div className="panel">
                <div className="alarm-form">
                  <div className="form-row">
                    <div>
                      <div className="field-label">Time</div>
                      <input type="time" className="time-input" value={time} onChange={e => setTime(e.target.value)} />
                    </div>
                    <div>
                      <div className="field-label">Repeat</div>
                      <div className="toggle-group">
                        <button className={`toggle-btn${!recurring ? " active" : ""}`} onClick={() => setRecurring(false)}>Once</button>
                        <button className={`toggle-btn${recurring && recurType === "daily" ? " active" : ""}`} onClick={() => { setRecurring(true); setRecurType("daily") }}>Daily</button>
                        <button className={`toggle-btn${recurring && recurType === "weekly" ? " active" : ""}`} onClick={() => { setRecurring(true); setRecurType("weekly") }}>Weekly</button>
                      </div>
                    </div>
                  </div>
                  {recurring && recurType === "weekly" && (
                    <div>
                      <div className="field-label">Days</div>
                      <div className="day-picker">
                        {DAYS.map((label, i) => (
                          <button key={i} className={`day-btn${recurDays.includes(i) ? " active" : ""}`} onClick={() => toggleDay(i)}>{label}</button>
                        ))}
                      </div>
                    </div>
                  )}
                  <div style={{ display: "flex", justifyContent: "flex-end" }}>
                    <button className="btn btn-accent" onClick={createAlarm}>+ Add Alarm</button>
                  </div>
                </div>
              </div>
            </div>

            <div className="section">
              <div className="section-header">
                <span className="section-label">Scheduled Alarms</span>
                <div className="section-line" />
              </div>
              {alarms.length === 0 ? (
                <div className="empty-state">no alarms for device {deviceId}</div>
              ) : (
                <div className="alarms-list">
                  {alarms.map(a => (
                    <div key={a.id} className="alarm-item">
                      <div>
                        <div className="alarm-time">{a.time}</div>
                        <div className="alarm-tags">
                          <span className={`tag ${a.enabled ? "tag-enabled" : "tag-disabled"}`}>{a.enabled ? "enabled" : "disabled"}</span>
                          <span className={`tag ${a.recurring ? "tag-recurring" : "tag-once"}`}>{recurLabel(a)}</span>
                          {a.recurring && a.recur_type === "weekly" && (a.recur_days || []).length > 0 && (
                            <span className="tag tag-once">{a.recur_days.map(d => DAYS[d]).join(" ")}</span>
                          )}
                        </div>
                      </div>
                      <div style={{ display: "flex", alignItems: "center", gap: "10px" }}>
                        <span className="alarm-id">#{a.id}</span>
                        <button className="btn btn-ghost" onClick={() => deleteAlarm(a.id)}>Delete</button>
                      </div>
                    </div>
                  ))}
                </div>
              )}
            </div>
          </>
        )}
      </div>

      {settingsDevice && (
        <DeviceSettingsModal
          device={settingsDevice}
          googleId={googleId}
          onClose={() => setSettingsDevice(null)}
          onSaved={updated => {
            setDevices(prev => prev.map(d => d.id === updated.id ? updated : d))
            showToast("Settings saved")
          }}
        />
      )}

      <div className={`toast${toastVisible ? " show" : ""}`}>{toast}</div>
    </>
  )
}

const pageStyles = `
  .dashboard { max-width: 960px; margin: 0 auto; padding: 48px 24px 80px; }
  .header { display: flex; align-items: flex-end; justify-content: space-between; margin-bottom: 56px; padding-bottom: 28px; border-bottom: 1px solid var(--border); flex-wrap: wrap; gap: 16px; }
  .header-title { font-size: 32px; font-weight: 800; letter-spacing: -1px; line-height: 1; color: var(--text); }
  .header-title span { color: var(--accent); }
  .header-sub { font-family: var(--mono); font-size: 11px; color: var(--text-muted); letter-spacing: 0.08em; text-transform: uppercase; margin-top: 6px; }
  .header-badge { font-family: var(--mono); font-size: 11px; padding: 5px 12px; border-radius: 100px; background: var(--surface); border: 1px solid var(--border); color: var(--text-muted); }
  .pair-btn { font-family: var(--sans); font-size: 12px; font-weight: 700; padding: 7px 14px; border-radius: 8px; background: var(--accent); color: #111; border: none; cursor: pointer; transition: opacity 0.15s; }
  .pair-btn:hover { opacity: 0.85; }
  .user-chip { display: flex; align-items: center; gap: 7px; padding: 5px 12px 5px 7px; border-radius: 100px; background: var(--surface); border: 1px solid var(--border); font-family: var(--mono); font-size: 11px; color: var(--text-sub); cursor: pointer; transition: border-color 0.15s; }
  .user-chip:hover { border-color: var(--danger); color: var(--danger); }
  .section { margin-bottom: 40px; }
  .section-header { display: flex; align-items: center; gap: 10px; margin-bottom: 16px; }
  .section-label { font-family: var(--mono); font-size: 10px; letter-spacing: 0.14em; text-transform: uppercase; color: var(--text-muted); white-space: nowrap; }
  .section-line { flex: 1; height: 1px; background: var(--border); }
  .devices-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(240px, 1fr)); gap: 12px; }
  .device-card { background: var(--surface); border: 1px solid var(--border); border-radius: var(--radius); padding: 18px 20px; cursor: pointer; transition: border-color 0.15s, transform 0.1s; position: relative; overflow: hidden; }
  .device-card:hover { border-color: rgba(255,238,140,0.3); transform: translateY(-1px); }
  .device-card.selected { border-color: var(--accent); background: var(--surface2); }
  .device-card-top { display: flex; align-items: center; justify-content: space-between; margin-bottom: 12px; }
  .device-id { font-family: var(--mono); font-size: 11px; color: var(--text-muted); }
  .status-pill { display: flex; align-items: center; gap: 5px; padding: 3px 9px; border-radius: 100px; font-family: var(--mono); font-size: 10px; font-weight: 500; letter-spacing: 0.06em; text-transform: uppercase; }
  .status-pill.online { background: var(--online-dim); color: var(--online); }
  .status-pill.offline { background: var(--offline-dim); color: var(--offline); }
  .status-dot { width: 6px; height: 6px; border-radius: 50%; background: currentColor; }
  .device-name { font-size: 17px; font-weight: 700; margin-bottom: 4px; letter-spacing: -0.3px; color: var(--text); }
  .device-seen { font-family: var(--mono); font-size: 10px; color: var(--text-muted); margin-bottom: 14px; }
  .device-actions { display: flex; align-items: center; justify-content: space-between; padding-top: 12px; border-top: 1px solid var(--border); }
  .settings-btn { background: none; border: none; color: var(--text-muted); font-family: var(--mono); font-size: 10px; cursor: pointer; padding: 0; letter-spacing: 0.04em; transition: color 0.12s; }
  .settings-btn:hover { color: var(--accent); }
  .panel { background: var(--surface); border: 1px solid var(--border); border-radius: var(--radius); padding: 22px 24px; }
  .panel-row { display: flex; align-items: center; justify-content: space-between; gap: 16px; flex-wrap: wrap; }
  .buzzer-title { font-size: 15px; font-weight: 700; color: var(--text); }
  .buzzer-sub { font-family: var(--mono); font-size: 10px; color: var(--text-muted); margin-top: 4px; }
  .btn-row { display: flex; gap: 8px; flex-wrap: wrap; }
  .btn { display: flex; align-items: center; gap: 7px; padding: 9px 18px; border-radius: 8px; font-family: var(--sans); font-size: 13px; font-weight: 600; cursor: pointer; border: 1px solid transparent; transition: all 0.15s; }
  .btn-on  { background: var(--online-dim);  border-color: rgba(34,216,122,0.3); color: var(--online); }
  .btn-on:hover  { background: rgba(34,216,122,0.22); border-color: var(--online); }
  .btn-off { background: var(--offline-dim); border-color: rgba(255,77,106,0.3); color: var(--offline); }
  .btn-off:hover { background: rgba(255,77,106,0.22); border-color: var(--offline); }
  .btn-accent { background: var(--accent); color: #111; border-color: var(--accent); font-weight: 700; }
  .btn-accent:hover { opacity: 0.85; }
  .btn-ghost { background: transparent; border-color: var(--border); color: var(--text-sub); }
  .btn-ghost:hover { border-color: var(--danger); color: var(--danger); background: rgba(255,77,106,0.08); }
  .alarm-form { display: flex; flex-direction: column; gap: 16px; }
  .form-row { display: flex; align-items: flex-end; gap: 16px; flex-wrap: wrap; }
  .field-label { font-family: var(--mono); font-size: 10px; color: var(--text-muted); letter-spacing: 0.1em; text-transform: uppercase; margin-bottom: 6px; }
  .time-input { background: var(--surface2); border: 1px solid var(--border); border-radius: 8px; color: var(--text); font-family: var(--mono); font-size: 22px; font-weight: 500; padding: 10px 16px; outline: none; width: 160px; color-scheme: dark; transition: border-color 0.15s; }
  .time-input:focus { border-color: var(--accent); }
  .toggle-group { display: flex; gap: 6px; }
  .toggle-btn { padding: 7px 14px; border-radius: 6px; font-family: var(--mono); font-size: 11px; font-weight: 500; cursor: pointer; border: 1px solid var(--border); background: transparent; color: var(--text-sub); transition: all 0.12s; }
  .toggle-btn.active { background: var(--accent-dim); border-color: var(--accent); color: var(--accent); }
  .day-picker { display: flex; gap: 5px; }
  .day-btn { width: 34px; height: 34px; border-radius: 6px; border: 1px solid var(--border); background: transparent; color: var(--text-sub); font-family: var(--mono); font-size: 10px; cursor: pointer; transition: all 0.12s; display: flex; align-items: center; justify-content: center; }
  .day-btn.active { background: var(--accent-dim); border-color: var(--accent); color: var(--accent); }
  .alarms-list { display: flex; flex-direction: column; gap: 8px; }
  .alarm-item { background: var(--surface); border: 1px solid var(--border); border-radius: 10px; padding: 14px 18px; display: flex; align-items: center; justify-content: space-between; transition: border-color 0.12s; animation: slideIn 0.2s ease; }
  .alarm-item:hover { border-color: rgba(255,238,140,0.2); }
  @keyframes slideIn { from{opacity:0;transform:translateY(-6px)} to{opacity:1;transform:translateY(0)} }
  .alarm-time { font-family: var(--mono); font-size: 20px; font-weight: 500; letter-spacing: 0.06em; color: var(--text); }
  .alarm-tags { display: flex; align-items: center; gap: 6px; flex-wrap: wrap; margin-top: 5px; }
  .tag { font-family: var(--mono); font-size: 9px; padding: 2px 7px; border-radius: 4px; text-transform: uppercase; letter-spacing: 0.06em; }
  .tag-recurring { color: var(--accent); background: var(--accent-dim); }
  .tag-once      { color: var(--text-muted); background: var(--surface2); border: 1px solid var(--border); }
  .tag-enabled   { color: var(--online); background: var(--online-dim); }
  .tag-disabled  { color: var(--text-muted); background: var(--surface2); }
  .alarm-id { font-family: var(--mono); font-size: 10px; color: var(--text-muted); }
  .empty-state { text-align: center; padding: 40px 20px; font-family: var(--mono); font-size: 12px; color: var(--text-muted); letter-spacing: 0.06em; background: var(--surface); border: 1px dashed var(--border); border-radius: var(--radius); }
  .toast { position: fixed; bottom: 28px; right: 28px; background: var(--surface2); border: 1px solid var(--border); border-radius: 8px; padding: 12px 18px; font-family: var(--mono); font-size: 12px; color: var(--text-sub); opacity: 0; transform: translateY(8px); transition: opacity 0.2s, transform 0.2s; pointer-events: none; z-index: 99; }
  .toast.show { opacity: 1; transform: translateY(0); }
  @media(max-width:600px){ .dashboard{padding:28px 16px 60px;} .header{flex-direction:column;align-items:flex-start;} .panel-row{flex-direction:column;align-items:flex-start;} .form-row{flex-direction:column;align-items:flex-start;} }
`