package handlers

import (
	"net/http"
	"pss/CloudClock/ws"

	"github.com/gorilla/websocket"
)

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool { return true },
}

func ConnectHandler(hub *ws.Hub) http.HandlerFunc {

	return func(w http.ResponseWriter, r *http.Request) {

		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			return
		}

		client := &ws.Client{
			Conn: conn,
			Send: make(chan any),
		}

		hub.Register <- client

		go client.WritePump()

	}

}
