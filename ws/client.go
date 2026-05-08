package ws

import "github.com/gorilla/websocket"

type Client struct {
	Conn *websocket.Conn
	Send chan any
}

func (c *Client) WritePump() {

	defer c.Conn.Close()

	for msg := range c.Send {
		c.Conn.WriteJSON(msg)
	}

}
