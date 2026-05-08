import { auth } from "@/auth"

export async function PATCH(req, { params }) {
  const session = await auth()
  if (!session) return Response.json({ error: "Unauthorized" }, { status: 401 })

  const body = await req.json()
  const res = await fetch(`${process.env.BACKEND_URL}/api/devices/${params.id}/settings`, {
    method: "PATCH",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  })
  const text = await res.text()
  return new Response(text, { status: res.status })
}