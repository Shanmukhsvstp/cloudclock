package models

import "time"

type Alarm struct {
	ID        string    `json:"id"`
	DeviceID  int64     `json:"device_id"`
	CreatedAt time.Time `json:"created_at"`
}
