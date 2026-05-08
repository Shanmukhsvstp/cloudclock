import NextAuth from "next-auth"
import Google from "next-auth/providers/google"

export const { handlers, auth, signIn, signOut } = NextAuth({
  providers: [
    Google({
      clientId: process.env.GOOGLE_CLIENT_ID,
      clientSecret: process.env.GOOGLE_CLIENT_SECRET,
    }),
  ],
  secret: process.env.NEXTAUTH_SECRET,
  session: { strategy: "jwt" },
  callbacks: {
    async jwt({ token, user }) {
      // On first login
      if (user) {
        token.googleId = user.id
        token.image = user.image
      }

      // Retry logic (IMPORTANT)
      if (!token.backendToken && token.googleId) {
        try {
          const res = await fetch(`${process.env.BACKEND_URL}/api/auth/token`, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ google_id: token.googleId }),
          })

          if (res.ok) {
            const data = await res.json()
            token.backendToken = data.token
            token.backendUserId = data.user_id
          } else {
            console.error("Backend token fetch failed", await res.text())
          }
        } catch (err) {
          console.error("Fetch error:", err)
        }
      }

      return token
    },
    async session({ session, token }) {
      session.user.googleId = token.googleId
      session.backendToken = token.backendToken
      session.backendUserId = token.backendUserId
      return session
    },
  },
  pages: { signIn: "/login" },
})