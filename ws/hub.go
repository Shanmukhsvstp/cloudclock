package ws

type Hub struct {
	Clients    map[*Client]bool
	Register   chan *Client
	Unregister chan *Client
	Broadcast  chan any
}

func NewHub() *Hub {

	return &Hub{
		Clients:    make(map[*Client]bool),
		Register:   make(chan *Client),
		Unregister: make(chan *Client),
		Broadcast:  make(chan any),
	}
}

func (h *Hub) Run() {

	for {

		select {

		case client := <-h.Register:
			h.Clients[client] = true

		case client := <-h.Unregister:
			delete(h.Clients, client)
			close(client.Send)

		case msg := <-h.Broadcast:

			for client := range h.Clients {
				client.Send <- msg
			}

		}

	}

}
