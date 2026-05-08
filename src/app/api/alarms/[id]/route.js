import { auth } from "@/auth"

export async function GET(req, { params }) {
  const session = await auth()
  if (!session) return Response.json({ error: "Unauthorized" }, { status: 401 })

  const res = await fetch(
    `${process.env.BACKEND_URL}/api/alarms/${params.id}`,
    { headers: { Authorization: `Bearer ${session.backendToken}` } }
  )
  const data = await res.json()
  return Response.json(data)
}

export async function POST(req) {
  const session = await auth()
  if (!session) return Response.json({ error: "Unauthorized" }, { status: 401 })

  const body = await req.json()
  const res = await fetch(`${process.env.BACKEND_URL}/api/alarms`, {
    method: "POST",
    headers: { "Content-Type": "application/json", ...authHeader(session) },
    body: JSON.stringify(body),
  })
  const text = await res.text()
  return new Response(text, { status: res.status })
}

export async function DELETE(req, { params }) {
  const session = await auth()
  if (!session) return Response.json({ error: "Unauthorized" }, { status: 401 })

  const res = await fetch(`${process.env.BACKEND_URL}/api/alarms/${params.id}`, {
    method: "DELETE",
    headers: { ...authHeader(session) },
  })
  const text = await res.text()
  return new Response(text, { status: res.status })
}