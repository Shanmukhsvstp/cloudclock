package models

type Device struct {
	ID     int64 `json:"id"`
	Online bool  `json:"online"`
}
