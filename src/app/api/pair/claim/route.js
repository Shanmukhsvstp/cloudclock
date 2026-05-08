import { auth } from "@/auth"

export async function POST(req) {
  const session = await auth()
  if (!session) return Response.json({ error: "Unauthorized" }, { status: 401 })

  const { code } = await req.json()
  if (!code || code.length !== 6) return Response.json({ error: "Invalid code" }, { status: 400 })

const res = await fetch(`${process.env.BACKEND_URL}/api/pair/claim`, {
  method: "POST",
  headers: { "Content-Type": "application/json", Authorization: `Bearer ${session.backendToken}` },
  body: JSON.stringify({ code: code.toUpperCase() }),
})
  const text = await res.text()
  return new Response(text, { status: res.status })
}