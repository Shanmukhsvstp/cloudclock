package models

import "time"

type DeviceState struct {
	ID        int       `json:"id"`
	DeviceID  int64     `json:"device_id"`
	Buzzer    bool      `json:"buzzer"`
	UpdatedAt time.Time `json:"updated_at"`
}
