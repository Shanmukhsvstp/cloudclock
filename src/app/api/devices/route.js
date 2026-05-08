import { auth } from "@/auth"

export async function GET() {
  const session = await auth()
  if (!session) return Response.json({ error: "Unauthorized" }, { status: 401 })

  const res = await fetch(`${process.env.BACKEND_URL}/api/devices`, {
    headers: { Authorization: `Bearer ${session.backendToken}` },
  })
  const data = await res.json()
  return Response.json(data)
}