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
    async jwt({ token, user, account }) {
      // if (user) {
      //   token.googleId = user.id
      //   token.image = user.image
      //   token.email = user.email
      //   token.name = user.name
      // }

      if (account?.provider === "google") {
        token.googleId = account.providerAccountId
        token.email = user.email
        token.name = user.name
        token.image = user.image
      }
        if (!token.backendToken && token.googleId) {
          const res = await fetch(`${process.env.BACKEND_URL}/auth/token`, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
              google_id: token.googleId,
              email: token.email,
              name: token.name,
            }),
          })

          if (res.ok) {
            const data = await res.json()
            token.backendToken = data.token
            token.backendUserId = data.user_id
          } else {
            console.error(await res.text())
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