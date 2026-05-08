import { auth } from "@/auth"

export async function POST(req) {
  const session = await auth()
  if (!session) return Response.json({ error: "Unauthorized" }, { status: 401 })

  const body = await req.json()
  const res = await fetch(`${process.env.BACKEND_URL}/api/buzzer`, {
    method: "POST",
    headers: { "Content-Type": "application/json", Authorization: `Bearer ${session.backendToken}` },
    body: JSON.stringify(body),
  })
  const text = await res.text()
  return new Response(text, { status: res.status })
}