package handlers

import (
	"context"
	"encoding/json"
	"net/http"

	"pss/CloudClock/models"
	"pss/CloudClock/ws"

	"github.com/jackc/pgx/v5/pgxpool"
)

func GetAlarms(db *pgxpool.Pool) http.HandlerFunc {

	return func(w http.ResponseWriter, r *http.Request) {

		rows, err := db.Query(context.Background(),
			`SELECT id, device_id FROM alarms ORDER BY id DESC`)
		if err != nil {
			http.Error(w, err.Error(), 500)
			return
		}

		var alarms []models.Alarm

		for rows.Next() {

			var a models.Alarm

			rows.Scan(&a.ID, &a.DeviceID)

			alarms = append(alarms, a)

		}

		json.NewEncoder(w).Encode(alarms)

	}

}

func CreateAlarm(db *pgxpool.Pool, hub *ws.Hub) http.HandlerFunc {

	return func(w http.ResponseWriter, r *http.Request) {

		var alarm models.Alarm

		err := json.NewDecoder(r.Body).Decode(&alarm)
		if err != nil {
			http.Error(w, err.Error(), 400)
			return
		}

		err = db.QueryRow(context.Background(),
			`INSERT INTO alarms (device_id)
			 VALUES ($1)
			 RETURNING id`,
			alarm.DeviceID,
		).Scan(&alarm.ID)

		if err != nil {
			http.Error(w, err.Error(), 500)
			return
		}

		hub.Broadcast <- map[string]any{
			"type":  "alarm_created",
			"alarm": alarm,
		}

		json.NewEncoder(w).Encode(alarm)

	}

}
