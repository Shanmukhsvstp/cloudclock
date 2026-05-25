"use client"
import { useState } from "react"
import { useSession } from "next-auth/react"
import { useRouter } from "next/navigation"

export default function PairPage() {
  const { data: session, status } = useSession()
  const router = useRouter()
  const [code, setCode] = useState("")
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState("")
  const [success, setSuccess] = useState(false)
  console.log("Session:", session, "Status:", status)
  const authHeader = session?.backendToken
    ? { Authorization: `Bearer ${session.backendToken}` }
    : {}

  if (status === "loading") return null
  if (!session) { router.push("/login"); return null }


  async function handlePair(e) {
    e.preventDefault()
    if (code.length !== 6) { setError("Code must be 6 characters"); return }
    setLoading(true)
    setError("")
    try {
      const res = await fetch(
        `${process.env.NEXT_PUBLIC_BACKEND_URL}/api/pair/claim`,
        {
          method: "POST",
          headers: {
            "Content-Type": "application/json",
            Authorization: `Bearer ${session?.backendToken}`,
          },
          body: JSON.stringify({ code: code.toUpperCase() }),
        }
      )
      if (!res.ok) {
        const data = await res.json().catch(() => ({}))
        setError(data.error || "Invalid or expired code")
      } else {
        setSuccess(true)
        setTimeout(() => router.push("/"), 1800)
      }
    } catch {
      setError("Network error, please try again")
    } finally {
      setLoading(false)
    }
  }

  return (
    <div style={styles.wrap}>
      <div style={styles.card}>
        <div style={styles.logo}><span style={{ color: "var(--accent)" }}>Cloud</span>Clock</div>
        <div style={styles.cardTitle}>Pair a Device</div>
        <p style={styles.desc}>
          Enter the 6-character code shown on your CloudClock display
        </p>

        {success ? (
          <div style={styles.successBox}>
            <span style={{ fontSize: 22 }}>✓</span>
            <span>Device paired! Redirecting...</span>
          </div>
        ) : (
          <form onSubmit={handlePair} style={styles.form}>
            <input
              style={{ ...styles.codeInput, ...(error ? styles.inputError : {}) }}
              value={code}
              onChange={e => {
                setError("")
                setCode(e.target.value.toUpperCase().replace(/[^A-Z0-9]/g, "").slice(0, 6))
              }}
              placeholder="A1B2C3"
              maxLength={6}
              autoFocus
              autoComplete="off"
              spellCheck={false}
            />
            {error && <p style={styles.errorText}>{error}</p>}
            <button
              type="submit"
              style={{ ...styles.btn, opacity: loading ? 0.6 : 1 }}
              disabled={loading || code.length !== 6}
            >
              {loading ? "Pairing..." : "Pair Device"}
            </button>
          </form>
        )}

        <button style={styles.backBtn} onClick={() => router.push("/")}>
          ← Back to dashboard
        </button>
      </div>
      <style>{`
        @keyframes fadeUp { from{opacity:0;transform:translateY(16px)} to{opacity:1;transform:translateY(0)} }
      `}</style>
    </div>
  )
}

const styles = {
  wrap: {
    minHeight: "100vh",
    display: "flex",
    alignItems: "center",
    justifyContent: "center",
    padding: "24px",
    background: "var(--bg)",
  },
  card: {
    background: "var(--surface)",
    border: "1px solid var(--border)",
    borderRadius: "20px",
    padding: "48px 40px",
    width: "100%",
    maxWidth: "420px",
    animation: "fadeUp 0.4s ease both",
    textAlign: "center",
  },
  logo: {
    fontFamily: "var(--sans)",
    fontSize: "22px",
    fontWeight: "800",
    letterSpacing: "-0.5px",
    marginBottom: "28px",
    color: "var(--text)",
  },
  cardTitle: {
    fontSize: "20px",
    fontWeight: "700",
    marginBottom: "12px",
    color: "var(--text)",
  },
  desc: {
    fontSize: "13px",
    color: "var(--text-sub)",
    lineHeight: 1.6,
    marginBottom: "32px",
  },
  form: { display: "flex", flexDirection: "column", gap: "12px" },
  codeInput: {
    background: "var(--surface2)",
    border: "1px solid var(--border)",
    borderRadius: "10px",
    color: "var(--text)",
    fontFamily: "var(--mono)",
    fontSize: "32px",
    fontWeight: "500",
    letterSpacing: "0.25em",
    padding: "14px 20px",
    textAlign: "center",
    outline: "none",
    width: "100%",
    transition: "border-color 0.15s",
  },
  inputError: { borderColor: "var(--danger)" },
  errorText: {
    fontFamily: "var(--mono)",
    fontSize: "11px",
    color: "var(--danger)",
    textAlign: "left",
  },
  btn: {
    background: "var(--accent)",
    color: "#111",
    border: "none",
    borderRadius: "10px",
    fontFamily: "var(--sans)",
    fontSize: "14px",
    fontWeight: "700",
    padding: "13px",
    cursor: "pointer",
    transition: "opacity 0.15s",
    marginTop: "4px",
  },
  successBox: {
    display: "flex",
    alignItems: "center",
    justifyContent: "center",
    gap: "10px",
    padding: "20px",
    background: "var(--online-dim)",
    border: "1px solid rgba(34,216,122,0.3)",
    borderRadius: "10px",
    color: "var(--online)",
    fontFamily: "var(--mono)",
    fontSize: "13px",
    marginBottom: "16px",
  },
  backBtn: {
    marginTop: "24px",
    background: "none",
    border: "none",
    color: "var(--text-muted)",
    fontFamily: "var(--mono)",
    fontSize: "11px",
    cursor: "pointer",
    letterSpacing: "0.06em",
  },
}