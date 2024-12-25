package main

import (
	"bytes"
	"context"
	"encoding/base64"
	"flag"
	"fmt"
	"log"
	"math/rand"
	"time"

	"github.com/gofiber/fiber/v2"
	"github.com/gofiber/swagger"

	models "ton-emulate-go/models"

	"github.com/go-redis/redis/v8"

	"github.com/vmihailenco/msgpack/v5"

	_ "ton-emulate-go/docs"
)

type TraceTask struct {
	ID           string `msgpack:"id"`
	BOC          string `msgpack:"boc"`
	IgnoreChksig bool   `msgpack:"ignore_chksig"`
}

type EmulateRequest struct {
	Boc          string `json:"boc" example:"te6ccgEBAQEAAgAAAA=="`
	IgnoreChksig bool   `json:"ignore_chksig" example:"false"`
}

// validate function for EmulateRequest
func (req EmulateRequest) Validate() error {
	if req.Boc == "" {
		return fmt.Errorf("boc is required")
	}
	_, err := base64.StdEncoding.Strict().DecodeString(req.Boc)
	return err
}

// Command-line flags
var (
	redisAddr  = flag.String("redis", "localhost:6379", "Redis server dsn")
	queueName  = flag.String("redis-queue", "somequeue", "Redis queue name")
	serverPort = flag.Int("port", 8080, "Server port")
	prefork    = flag.Bool("prefork", false, "Use prefork")
)

func generateTaskID() string {
	const letters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
	b := make([]byte, 10)
	for i := range b {
		b[i] = letters[rand.Intn(len(letters))]
	}
	return string(b)
}

// @title TON Emulate API
// @version 0.0.1
// @description	TON Emulate API provides an endpoint to emulate transactions and traces before committing them to the blockchain.
// @basePath /emulate

// EmulateTrace godoc
// @Summary Emulate trace by external message
// @Schemes
// @Description Emulate trace by external message.
// @Tags emulate
// @Accept json
// @Produce json
// @Param   request     body    EmulateRequest     true        "External Message Request"
// @Router /v1/emulateTrace [post]
func emulateTrace(c *fiber.Ctx) error {
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	var req EmulateRequest
	if err := c.BodyParser(&req); err != nil {
		return fiber.NewError(fiber.StatusBadRequest, "invalid request: "+err.Error())
	}
	if err := req.Validate(); err != nil {
		return fiber.NewError(fiber.StatusBadRequest, "invalid request: "+err.Error())
	}

	taskID := generateTaskID()
	task := TraceTask{
		ID:           taskID,
		BOC:          req.Boc,
		IgnoreChksig: req.IgnoreChksig,
	}

	// Serialize the task using msgpack
	var buf bytes.Buffer
	enc := msgpack.NewEncoder(&buf)
	enc.UseArrayEncodedStructs(false)

	if err := enc.Encode(task); err != nil {
		return fiber.NewError(fiber.StatusBadRequest, "failed to serialize task: "+err.Error())
	}

	// Initialize Redis client
	rdb := redis.NewClient(&redis.Options{
		Addr: *redisAddr, // Redis server address
	})

	// Push the packed task to the Redis queue
	if err := rdb.LPush(ctx, *queueName, buf.Bytes()).Err(); err != nil {
		return fiber.NewError(fiber.StatusInternalServerError, "failed to push task to Redis: "+err.Error())
	}

	// Subscribe to the result channel
	pubsub := rdb.Subscribe(ctx, "result_channel_"+taskID)
	defer pubsub.Close()

	// Wait for the result
	msg, err := pubsub.ReceiveMessage(ctx)
	if err != nil {
		return fiber.NewError(fiber.StatusInternalServerError, "failed to receive result from Redis: "+err.Error())
	}

	if msg.Payload == "error" {
		error_msg, err := rdb.Get(ctx, "error_channel_"+taskID).Result()
		if err != nil {
			return fiber.NewError(fiber.StatusInternalServerError, "failed to receive error from Redis: "+err.Error())
		}
		return fiber.NewError(fiber.StatusInternalServerError, error_msg)
	}
	if msg.Payload != "success" {
		return fiber.NewError(fiber.StatusInternalServerError, "unexpected message from Redis: "+msg.Payload)
	}

	hset, err := rdb.HGetAll(ctx, "result_hset_"+taskID).Result()
	if err != nil {
		return fiber.NewError(fiber.StatusInternalServerError, "failed to get result from Redis: "+err.Error())
	}

	result, err := models.TransformToAPIResponse(hset)
	if err != nil {
		return fiber.NewError(fiber.StatusInternalServerError, "failed to transform result: "+err.Error())
	}

	return c.Status(200).JSON(result)
}

func main() {
	flag.Parse()

	config := fiber.Config{
		AppName:        "TON Index API",
		Concurrency:    256 * 1024,
		Prefork:        *prefork,
		ReadBufferSize: 1048576,
	}
	app := fiber.New(config)

	app.Use(func(c *fiber.Ctx) error {
		err := c.Next()
		if err != nil {
			// Log the error internally here if necessary

			// Return a JSON response with the error
			code := fiber.StatusInternalServerError
			if e, ok := err.(*fiber.Error); ok {
				code = e.Code
			}

			return c.Status(code).JSON(fiber.Map{
				"error": err.Error(),
			})
		}
		return nil
	})

	app.Post("/emulate/v1/emulateTrace", emulateTrace)

	var swagger_config = swagger.Config{
		Title:           "TON Emulate API - Swagger UI",
		Layout:          "BaseLayout",
		DeepLinking:     true,
		TryItOutEnabled: true,
	}
	app.Get("/emulate/*", swagger.New(swagger_config))
	bind := fmt.Sprintf(":%d", *serverPort)
	err := app.Listen(bind)
	log.Fatal(err)
}
