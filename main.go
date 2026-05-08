package main

import (
	"crypto/rand"
	"database/sql"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"

	"github.com/gofiber/contrib/websocket"
	"github.com/gofiber/fiber/v2"
	"github.com/gofiber/fiber/v2/middleware/cors"
	"github.com/gofiber/fiber/v2/middleware/logger"
	"github.com/golang-jwt/jwt/v5"
	_ "github.com/jackc/pgx/v5/stdlib"
)

// ─── Types ───────────────────────────────────────────────────────────────────

type Device struct {
	Conn *websocket.Conn
}

// ─── Globals ─────────────────────────────────────────────────────────────────

var (
	devices      = map[int]*Device{}
	devicesMutex sync.RWMutex
)

var (
	jwtDeviceSecret  = []byte("super-secret-key")         // existing device tokens
	jwtSessionSecret = []byte("super-secret-session-key") // new user session tokens
)

var db *sql.DB

// Google OAuth config — set via env or hardcode for dev
const (
	googleClientID     = "YOUR_GOOGLE_CLIENT_ID"
	googleClientSecret = "YOUR_GOOGLE_CLIENT_SECRET"
	googleRedirectURI  = "http://localhost:8080/auth/google/callback"
	frontendURL        = "http://localhost:3000"
)

// ─── Main ─────────────────────────────────────────────────────────────────────

func main() {
	var err error
	db, err = sql.Open(
		"pgx",
		"postgresql://neondb_owner:npg_Bn2JxPyUpkq7@ep-still-term-anelgt04-pooler.c-6.us-east-1.aws.neon.tech/neondb?sslmode=require&channel_binding=require",
	)
	if err != nil {
		log.Fatal(err)
	}
	if err = db.Ping(); err != nil {
		log.Fatal("Database connection failed:", err)
	}
	log.Println("Database connected")

	db.Exec(`UPDATE devices SET online=false`)

	go alarmChecker()
	go expiredCodeCleaner()

	app := fiber.New()

	app.Use(logger.New(logger.Config{
		Format:     "[${time}] ${status} - ${latency} ${method} ${path}\n",
		TimeFormat: "2006-01-02 15:04:05",
		TimeZone:   "Local",
	}))

	app.Use(cors.New(cors.Config{
		AllowOrigins:     frontendURL,
		AllowHeaders:     "Origin, Content-Type, Accept, Authorization",
		AllowCredentials: true,
	}))

	// ── WebSocket (device auth) ──
	app.Get("/ws", websocket.New(handleWS))

	// ── Google OAuth ──
	app.Get("/auth/google", handleGoogleLogin)
	app.Get("/auth/google/callback", handleGoogleCallback)
	app.Get("/auth/me", requireUser, handleMe)
	app.Post("/auth/logout", handleLogout)

	app.Post("/auth/token", handleExchangeToken)

	// ── Pairing ──
	app.Get("/api/pair/code", requireDevice, handleGetPairingCode)
	app.Post("/api/pair/claim", requireUser, handleClaimPairingCode)

	// ── Device management (user-scoped) ──
	app.Get("/api/devices", requireUser, listDevices)
	app.Patch("/api/devices/:id/settings", requireUser, updateDeviceSettings)

	// ── Alarms (user-scoped) ──
	app.Post("/api/alarms", requireUser, createAlarm)
	app.Delete("/api/alarms/:id", requireUser, deleteAlarm)
	app.Get("/api/alarms/:device_id", requireUser, listAlarms)

	// ── Buzzer (user-scoped) ──
	app.Post("/api/buzzer", requireUser, triggerBuzzer)

	// ── Admin: register device (no auth — called once during provisioning) ──
	app.Post("/api/register_device", registerDevice)

	log.Fatal(app.Listen(":4002"))
}

func pushAlarmsToDevice(deviceID int) {
	rows, err := db.Query(`
        SELECT id, alarm_time, enabled, recurring, recur_type, recur_days
        FROM alarms WHERE device_id=$1`, deviceID)
	if err != nil {
		return
	}
	defer rows.Close()

	var alarmList []map[string]interface{}
	for rows.Next() {
		var id int
		var t string
		var enabled, recurring bool
		var recurType sql.NullString
		var recurDays []byte
		rows.Scan(&id, &t, &enabled, &recurring, &recurType, &recurDays)

		var days []int
		if recurDays != nil {
			json.Unmarshal(recurDays, &days)
		}
		if days == nil {
			days = []int{}
		}
		alarmList = append(alarmList, map[string]interface{}{
			"id":         id,
			"time":       t,
			"enabled":    enabled,
			"recurring":  recurring,
			"recur_type": recurType.String,
			"recur_days": days,
		})
	}
	if alarmList == nil {
		alarmList = []map[string]interface{}{}
	}

	sendToDevice(deviceID, map[string]interface{}{
		"type":   "sync_alarms",
		"alarms": alarmList,
	})
}

func handleExchangeToken(c *fiber.Ctx) error {
	var body struct {
		GoogleID string `json:"google_id"`
		Email    string `json:"email"`
		Name     string `json:"name"`
	}
	if err := c.BodyParser(&body); err != nil || body.GoogleID == "" {
		println("Token exchange error: invalid body or missing google_id")
		return c.Status(400).JSON(fiber.Map{"error": "missing google_id"})
	}

	var userID int
	err := db.QueryRow(`SELECT id FROM users WHERE google_id=$1`, body.GoogleID).Scan(&userID)

	if err != nil {
		// create user instead of failing
		err = db.QueryRow(
			`INSERT INTO users (google_id, email, name)
	 			VALUES ($1, $2, $3)
	 			RETURNING id`,
			body.GoogleID,
			body.Email,
			body.Name,
		).Scan(&userID)

		if err != nil {
			log.Println("User creation error:", err)
			return c.Status(500).JSON(fiber.Map{"error": "user create failed"})
		}
	}
	token, err := generateSessionToken(userID)
	if err != nil {
		println("Token generation error:", err)
		return c.Status(500).JSON(fiber.Map{"error": "token error"})
	}

	return c.JSON(fiber.Map{"token": token, "user_id": userID})
}

// ─── WebSocket Handler ────────────────────────────────────────────────────────

func handleWS(c *websocket.Conn) {
	token := c.Query("token")
	deviceID := validateDeviceToken(token)
	if deviceID == 0 {
		log.Println("Invalid device token")
		c.Close()
		return
	}

	db.Exec(`UPDATE devices SET online=true, last_seen=NOW() WHERE id=$1`, deviceID)
	log.Println("Device connected:", deviceID)

	devicesMutex.Lock()
	if old, exists := devices[deviceID]; exists {
		old.Conn.Close()
	}
	devices[deviceID] = &Device{Conn: c}
	devicesMutex.Unlock()

	// On connect: send current settings + alarm sync
	sendDeviceSettings(deviceID)
	pushAlarmsToDevice(deviceID)
	pushAlarmsToDevice(deviceID)

	// Check if this device is already paired
	var paired bool
	db.QueryRow(`SELECT paired FROM devices WHERE id=$1`, deviceID).Scan(&paired)
	if !paired {
		sendToDevice(deviceID, map[string]interface{}{"type": "request_pairing_code"})
	}

	defer func() {
		devicesMutex.Lock()
		delete(devices, deviceID)
		devicesMutex.Unlock()
		db.Exec(`UPDATE devices SET online=false WHERE id=$1`, deviceID)
		log.Println("Device disconnected:", deviceID)
	}()

	for {
		_, msg, err := c.ReadMessage()
		if err != nil {
			break
		}
		var data map[string]interface{}
		json.Unmarshal(msg, &data)
		msgType, _ := data["type"].(string)

		switch msgType {
		case "heartbeat":
			db.Exec(`UPDATE devices SET last_seen=NOW() WHERE id=$1`, deviceID)
		case "buzzer_status":
			value, _ := data["value"].(bool)
			db.Exec(`UPDATE device_state SET buzzer=$1, updated_at=NOW() WHERE device_id=$2`, value, deviceID)
		}
	}
}

// ─── Auth Middleware ──────────────────────────────────────────────────────────

func requireUser(c *fiber.Ctx) error {
	authHeader := c.Get("Authorization")
	tokenStr := strings.TrimPrefix(authHeader, "Bearer ")
	if tokenStr == "" {
		log.Printf("auth fail: no Authorization header, path=%s", c.Path())
		return c.Status(401).JSON(fiber.Map{"error": "unauthorized"})
	}
	userID := validateSessionToken(tokenStr)
	if userID == 0 {
		log.Printf("auth fail: bad token, path=%s", c.Path())
		return c.Status(401).JSON(fiber.Map{"error": "invalid token"})
	}
	c.Locals("userID", userID)
	return c.Next()
}

func requireDevice(c *fiber.Ctx) error {
	authHeader := c.Get("Authorization")
	tokenStr := strings.TrimPrefix(authHeader, "Bearer ")
	if tokenStr == "" {
		return c.Status(401).JSON(fiber.Map{"error": "unauthorized"})
	}
	deviceID := validateDeviceToken(tokenStr)
	if deviceID == 0 {
		return c.Status(401).JSON(fiber.Map{"error": "invalid device token"})
	}
	c.Locals("deviceID", deviceID)
	return c.Next()
}

// ─── Google OAuth ─────────────────────────────────────────────────────────────

func handleGoogleLogin(c *fiber.Ctx) error {
	authURL := fmt.Sprintf(
		"https://accounts.google.com/o/oauth2/v2/auth?client_id=%s&redirect_uri=%s&response_type=code&scope=openid%%20email%%20profile",
		googleClientID, url.QueryEscape(googleRedirectURI),
	)
	return c.Redirect(authURL, 302)
}

func handleGoogleCallback(c *fiber.Ctx) error {
	code := c.Query("code")
	if code == "" {
		return c.Status(400).SendString("missing code")
	}

	// Exchange code for tokens
	resp, err := http.PostForm("https://oauth2.googleapis.com/token", url.Values{
		"code":          {code},
		"client_id":     {googleClientID},
		"client_secret": {googleClientSecret},
		"redirect_uri":  {googleRedirectURI},
		"grant_type":    {"authorization_code"},
	})
	if err != nil || resp.StatusCode != 200 {
		return c.Status(500).SendString("failed to exchange token")
	}
	defer resp.Body.Close()

	var tokenResp struct {
		AccessToken string `json:"access_token"`
	}
	json.NewDecoder(resp.Body).Decode(&tokenResp)

	// Fetch user info
	req, _ := http.NewRequest("GET", "https://www.googleapis.com/oauth2/v2/userinfo", nil)
	req.Header.Set("Authorization", "Bearer "+tokenResp.AccessToken)
	userResp, err := http.DefaultClient.Do(req)
	if err != nil {
		return c.Status(500).SendString("failed to get user info")
	}
	defer userResp.Body.Close()
	body, _ := io.ReadAll(userResp.Body)

	var googleUser struct {
		ID      string `json:"id"`
		Email   string `json:"email"`
		Name    string `json:"name"`
		Picture string `json:"picture"`
	}
	json.Unmarshal(body, &googleUser)

	// Upsert user
	var userID int
	err = db.QueryRow(`
		INSERT INTO users(google_id, email, name, avatar)
		VALUES($1, $2, $3, $4)
		ON CONFLICT(google_id) DO UPDATE
		  SET email=EXCLUDED.email, name=EXCLUDED.name, avatar=EXCLUDED.avatar
		RETURNING id`,
		googleUser.ID, googleUser.Email, googleUser.Name, googleUser.Picture,
	).Scan(&userID)
	if err != nil {
		return c.Status(500).SendString("db error: " + err.Error())
	}

	sessionToken, err := generateSessionToken(userID)
	if err != nil {
		return c.Status(500).SendString("token error")
	}

	// Redirect to frontend with token
	return c.Redirect(frontendURL+"/auth/callback?token="+sessionToken, 302)
}

func handleMe(c *fiber.Ctx) error {
	userID := c.Locals("userID").(int)
	var u struct {
		ID     int    `json:"id"`
		Email  string `json:"email"`
		Name   string `json:"name"`
		Avatar string `json:"avatar"`
	}
	db.QueryRow(`SELECT id, email, name, avatar FROM users WHERE id=$1`, userID).
		Scan(&u.ID, &u.Email, &u.Name, &u.Avatar)
	return c.JSON(u)
}

func handleLogout(c *fiber.Ctx) error {
	return c.JSON(fiber.Map{"ok": true})
}

// ─── Pairing ──────────────────────────────────────────────────────────────────

func handleGetPairingCode(c *fiber.Ctx) error {
	deviceID := c.Locals("deviceID").(int)

	// Delete any existing code for this device
	db.Exec(`DELETE FROM pairing_codes WHERE device_id=$1`, deviceID)

	code := generatePairingCode()
	expires := time.Now().Add(10 * time.Minute)

	_, err := db.Exec(
		`INSERT INTO pairing_codes(code, device_id, expires_at) VALUES($1, $2, $3)`,
		code, deviceID, expires,
	)
	if err != nil {
		return c.Status(500).JSON(fiber.Map{"error": "failed to create code"})
	}

	return c.JSON(fiber.Map{
		"code":       code,
		"expires_at": expires,
	})
}

func handleClaimPairingCode(c *fiber.Ctx) error {
	userID := c.Locals("userID").(int)

	var body struct {
		Code string `json:"code"`
	}
	if err := c.BodyParser(&body); err != nil {
		return c.Status(400).JSON(fiber.Map{"error": "invalid body"})
	}
	body.Code = strings.ToUpper(strings.TrimSpace(body.Code))

	var deviceID int
	var expiresAt time.Time
	err := db.QueryRow(
		`SELECT device_id, expires_at FROM pairing_codes WHERE code=$1`,
		body.Code,
	).Scan(&deviceID, &expiresAt)
	if err != nil {
		return c.Status(404).JSON(fiber.Map{"error": "invalid code"})
	}
	if time.Now().After(expiresAt) {
		db.Exec(`DELETE FROM pairing_codes WHERE code=$1`, body.Code)
		return c.Status(410).JSON(fiber.Map{"error": "code expired"})
	}

	// Link device to user
	_, err = db.Exec(
		`UPDATE devices SET user_id=$1, paired=true WHERE id=$2`,
		userID, deviceID,
	)
	if err != nil {
		return c.Status(500).JSON(fiber.Map{"error": "failed to pair"})
	}

	db.Exec(`DELETE FROM pairing_codes WHERE code=$1`, body.Code)

	// Notify device via WebSocket
	sendToDevice(deviceID, map[string]interface{}{"type": "pairing_complete"})

	// Return device info
	var d struct {
		ID   int    `json:"id"`
		Name string `json:"name"`
	}
	db.QueryRow(`SELECT id, name FROM devices WHERE id=$1`, deviceID).Scan(&d.ID, &d.Name)

	return c.JSON(fiber.Map{"ok": true, "device": d})
}

// ─── Device Settings ──────────────────────────────────────────────────────────

func updateDeviceSettings(c *fiber.Ctx) error {
	userID := c.Locals("userID").(int)
	deviceIDStr := c.Params("id")

	// Ownership check
	var ownerID sql.NullInt64
	err := db.QueryRow(`SELECT user_id FROM devices WHERE id=$1`, deviceIDStr).Scan(&ownerID)
	if err != nil || !ownerID.Valid || int(ownerID.Int64) != userID {
		return c.Status(403).JSON(fiber.Map{"error": "forbidden"})
	}

	var body struct {
		HourFormat int    `json:"hour_format"`
		Timezone   string `json:"timezone"`
	}
	if err := c.BodyParser(&body); err != nil {
		return c.Status(400).JSON(fiber.Map{"error": "invalid body"})
	}
	if body.HourFormat != 12 && body.HourFormat != 24 {
		body.HourFormat = 24
	}

	_, err = db.Exec(
		`UPDATE devices SET hour_format=$1, timezone=$2 WHERE id=$3`,
		body.HourFormat, body.Timezone, deviceIDStr,
	)
	if err != nil {
		return c.Status(500).JSON(fiber.Map{"error": "db error"})
	}

	// Push settings to device if online
	var deviceID int
	fmt.Sscanf(deviceIDStr, "%d", &deviceID)
	sendDeviceSettings(deviceID)

	return c.JSON(fiber.Map{"ok": true})
}

func sendDeviceSettings(deviceID int) {
	var hourFormat int
	var timezone string
	db.QueryRow(`SELECT hour_format, timezone FROM devices WHERE id=$1`, deviceID).
		Scan(&hourFormat, &timezone)
	sendToDevice(deviceID, map[string]interface{}{
		"type":        "sync_settings",
		"hour_format": hourFormat,
		"timezone":    timezone,
	})
}

// ─── Device List ──────────────────────────────────────────────────────────────

func listDevices(c *fiber.Ctx) error {
	userID := c.Locals("userID").(int)

	rows, err := db.Query(`
		SELECT id, name, last_seen, hour_format, timezone, paired
		FROM devices WHERE user_id=$1`,
		userID,
	)
	if err != nil {
		return err
	}
	defer rows.Close()

	var result []fiber.Map
	for rows.Next() {
		var id, hourFormat int
		var name, timezone string
		var lastSeen sql.NullString
		var paired bool
		rows.Scan(&id, &name, &lastSeen, &hourFormat, &timezone, &paired)

		devicesMutex.RLock()
		_, online := devices[id]
		devicesMutex.RUnlock()

		result = append(result, fiber.Map{
			"id":          id,
			"name":        name,
			"online":      online,
			"last_seen":   lastSeen.String,
			"hour_format": hourFormat,
			"timezone":    timezone,
			"paired":      paired,
		})
	}
	if result == nil {
		result = []fiber.Map{}
	}
	return c.JSON(result)
}

// ─── Alarms (ownership-checked) ───────────────────────────────────────────────

func createAlarm(c *fiber.Ctx) error {
	userID := c.Locals("userID").(int)

	type Req struct {
		DeviceID  int    `json:"device_id"`
		Time      string `json:"time"`
		Recurring bool   `json:"recurring"`
		RecurType string `json:"recur_type"`
		RecurDays []int  `json:"recur_days"`
	}
	var body Req
	c.BodyParser(&body)

	if !userOwnsDevice(userID, body.DeviceID) {
		return c.Status(403).JSON(fiber.Map{"error": "forbidden"})
	}

	var recurType interface{} = nil
	var recurDays interface{} = nil
	if body.Recurring {
		recurType = body.RecurType
		if body.RecurType == "weekly" && len(body.RecurDays) > 0 {
			recurDays = body.RecurDays
		}
	}

	_, err := db.Exec(`
		INSERT INTO alarms(device_id, alarm_time, recurring, recur_type, recur_days)
		VALUES($1, $2, $3, $4, $5)`,
		body.DeviceID, body.Time, body.Recurring, recurType, recurDays,
	)
	if err != nil {
		return c.Status(500).SendString("failed: " + err.Error())
	}

	sendToDevice(body.DeviceID, map[string]interface{}{"type": "sync_alarms"})
	pushAlarmsToDevice(body.DeviceID)
	return c.SendString("ok")
}

func deleteAlarm(c *fiber.Ctx) error {
	userID := c.Locals("userID").(int)
	id := c.Params("id")

	var deviceID int
	err := db.QueryRow(`SELECT device_id FROM alarms WHERE id=$1`, id).Scan(&deviceID)
	if err != nil {
		return c.Status(404).SendString("alarm not found")
	}
	if !userOwnsDevice(userID, deviceID) {
		return c.Status(403).JSON(fiber.Map{"error": "forbidden"})
	}

	db.Exec(`DELETE FROM alarms WHERE id=$1`, id)
	pushAlarmsToDevice(deviceID)
	return c.SendString("deleted")
}

func listAlarms(c *fiber.Ctx) error {
	userID := c.Locals("userID").(int)
	deviceIDStr := c.Params("device_id")

	var deviceID int
	fmt.Sscanf(deviceIDStr, "%d", &deviceID)

	if !userOwnsDevice(userID, deviceID) {
		return c.Status(403).JSON(fiber.Map{"error": "forbidden"})
	}

	rows, err := db.Query(`
		SELECT id, alarm_time, enabled, recurring, recur_type, recur_days
		FROM alarms WHERE device_id=$1`, deviceIDStr)
	if err != nil {
		return err
	}
	defer rows.Close()

	var alarms []fiber.Map
	for rows.Next() {
		var id int
		var t string
		var enabled, recurring bool
		var recurType sql.NullString
		var recurDays []byte
		rows.Scan(&id, &t, &enabled, &recurring, &recurType, &recurDays)

		var days []int
		if recurDays != nil {
			json.Unmarshal(recurDays, &days)
		}
		alarms = append(alarms, fiber.Map{
			"id": id, "time": t, "enabled": enabled,
			"recurring": recurring, "recur_type": recurType.String, "recur_days": days,
		})
	}
	if alarms == nil {
		alarms = []fiber.Map{}
	}
	return c.JSON(alarms)
}

// ─── Buzzer ───────────────────────────────────────────────────────────────────

func triggerBuzzer(c *fiber.Ctx) error {
	userID := c.Locals("userID").(int)

	type Req struct {
		DeviceID int  `json:"device_id"`
		State    bool `json:"state"`
	}
	var body Req
	if err := c.BodyParser(&body); err != nil {
		return c.Status(400).JSON(fiber.Map{"error": "invalid body"})
	}
	if !userOwnsDevice(userID, body.DeviceID) {
		return c.Status(403).JSON(fiber.Map{"error": "forbidden"})
	}

	err := sendToDevice(body.DeviceID, map[string]interface{}{
		"type": "buzzer", "value": body.State,
	})
	if err != nil {
		return c.Status(404).SendString("device offline")
	}
	return c.SendString("ok")
}

// ─── Register Device (provisioning only) ─────────────────────────────────────

func registerDevice(c *fiber.Ctx) error {
	type Req struct {
		Name string `json:"name"`
	}
	var body Req
	if err := c.BodyParser(&body); err != nil {
		return err
	}
	var deviceID int
	err := db.QueryRow(`INSERT INTO devices(name) VALUES($1) RETURNING id`, body.Name).Scan(&deviceID)
	if err != nil {
		return c.Status(500).SendString("failed: " + err.Error())
	}
	// Also create device_state row
	db.Exec(`INSERT INTO device_state(device_id) VALUES($1)`, deviceID)

	token, err := generateDeviceToken(deviceID)
	if err != nil {
		return c.Status(500).SendString("token error")
	}
	return c.JSON(fiber.Map{"device_id": deviceID, "token": token})
}

// ─── Alarm Checker ────────────────────────────────────────────────────────────

func alarmChecker() {
	for {
		now := time.Now()
		next := now.Truncate(time.Minute).Add(time.Minute)
		time.Sleep(time.Until(next))
		checkAndFireAlarms()
	}
}

func checkAndFireAlarms() {
	now := time.Now()
	currentTime := now.Format("15:04")
	currentDay := int(now.Weekday())

	rows, err := db.Query(`
		SELECT id, device_id, recurring, recur_type, recur_days
		FROM alarms WHERE alarm_time=$1 AND enabled=true`, currentTime)
	if err != nil {
		return
	}
	defer rows.Close()

	type AlarmRow struct {
		ID        int
		DeviceID  int
		Recurring bool
		RecurType sql.NullString
		RecurDays []int64
	}

	var toFire []AlarmRow
	var toDelete []int

	for rows.Next() {
		var a AlarmRow
		var recurDays []byte
		rows.Scan(&a.ID, &a.DeviceID, &a.Recurring, &a.RecurType, &recurDays)
		if recurDays != nil {
			json.Unmarshal(recurDays, &a.RecurDays)
		}

		shouldFire := false
		if !a.Recurring {
			shouldFire = true
			toDelete = append(toDelete, a.ID)
		} else if a.RecurType.String == "daily" {
			shouldFire = true
		} else if a.RecurType.String == "weekly" {
			for _, d := range a.RecurDays {
				if int(d) == currentDay {
					shouldFire = true
					break
				}
			}
		}
		if shouldFire {
			toFire = append(toFire, a)
		}
	}

	for _, id := range toDelete {
		db.Exec(`DELETE FROM alarms WHERE id=$1`, id)
	}
	for _, a := range toFire {
		sendToDevice(a.DeviceID, map[string]interface{}{"type": "alarm_fire", "alarm_id": a.ID})
		sendToDevice(a.DeviceID, map[string]interface{}{"type": "sync_alarms"})
	}
}

// ─── Expired Code Cleaner ─────────────────────────────────────────────────────

func expiredCodeCleaner() {
	for {
		time.Sleep(1 * time.Minute)
		db.Exec(`DELETE FROM pairing_codes WHERE expires_at < NOW()`)
	}
}

// ─── Helpers ──────────────────────────────────────────────────────────────────

func sendToDevice(deviceID int, payload map[string]interface{}) error {
	log.Printf("Attempting WS send to device %d", deviceID)
	devicesMutex.RLock()
	device := devices[deviceID]
	devicesMutex.RUnlock()
	if device == nil {
		return fmt.Errorf("device offline")
	}
	return device.Conn.WriteJSON(payload)
}

func userOwnsDevice(userID, deviceID int) bool {
	var ownerID sql.NullInt64
	err := db.QueryRow(`SELECT user_id FROM devices WHERE id=$1`, deviceID).Scan(&ownerID)
	return err == nil && ownerID.Valid && int(ownerID.Int64) == userID
}

func generatePairingCode() string {
	const chars = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789" // no ambiguous chars (0,O,1,I)
	b := make([]byte, 6)
	rand.Read(b)
	for i := range b {
		b[i] = chars[int(b[i])%len(chars)]
	}
	return string(b)
}

func generateDeviceToken(deviceID int) (string, error) {
	claims := jwt.MapClaims{"device_id": deviceID, "sub": "device"}
	token := jwt.NewWithClaims(jwt.SigningMethodHS256, claims)
	return token.SignedString(jwtDeviceSecret)
}

func generateSessionToken(userID int) (string, error) {
	claims := jwt.MapClaims{
		"user_id": userID,
		"sub":     "user",
		"exp":     time.Now().Add(30 * 24 * time.Hour).Unix(),
	}
	token := jwt.NewWithClaims(jwt.SigningMethodHS256, claims)
	return token.SignedString(jwtSessionSecret)
}

func validateDeviceToken(tokenString string) int {
	token, err := jwt.Parse(tokenString, func(t *jwt.Token) (interface{}, error) {
		if _, ok := t.Method.(*jwt.SigningMethodHMAC); !ok {
			return nil, jwt.ErrSignatureInvalid
		}
		return jwtDeviceSecret, nil
	})
	if err != nil || !token.Valid {
		return 0
	}
	claims, ok := token.Claims.(jwt.MapClaims)
	if !ok {
		return 0
	}
	id, ok := claims["device_id"].(float64)
	if !ok {
		return 0
	}
	return int(id)
}

func validateSessionToken(tokenString string) int {
	token, err := jwt.Parse(tokenString, func(t *jwt.Token) (interface{}, error) {
		if _, ok := t.Method.(*jwt.SigningMethodHMAC); !ok {
			return nil, jwt.ErrSignatureInvalid
		}
		return jwtSessionSecret, nil
	})
	if err != nil || !token.Valid {
		return 0
	}
	claims, ok := token.Claims.(jwt.MapClaims)
	if !ok {
		return 0
	}
	id, ok := claims["user_id"].(float64)
	if !ok {
		return 0
	}
	return int(id)
}
