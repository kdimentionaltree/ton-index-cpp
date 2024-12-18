package main

import (
	"bytes"
	"context"
	"math/rand"
	"net/http"
	"time"

	"github.com/gin-gonic/gin"
	swaggerfiles "github.com/swaggo/files"
	ginSwagger "github.com/swaggo/gin-swagger"

	docs "ton-emulate-go/docs"
	models "ton-emulate-go/models"

	"github.com/go-redis/redis/v8"

	"github.com/vmihailenco/msgpack/v5"
)

type TraceTask struct {
	ID  string `msgpack:"id"`
	BOC string `msgpack:"boc"`
}

type EmulateRequest struct {
	Boc string `json:"boc"`
}

func generateTaskID() string {
	rand.Seed(time.Now().UnixNano())
	const letters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
	b := make([]byte, 10)
	for i := range b {
		b[i] = letters[rand.Intn(len(letters))]
	}
	return string(b)
}

// @BasePath /api/v1

// EmulateTrace godoc
// @Summary Emulate trace by external message
// @Schemes
// @Description Emulate trace by external message.
// @Tags emulate
// @Accept json
// @Produce json
// @Param   boc     body    object     true        "External Message BOC"
// @Success 200 {string} Helloworld
// @Router /emulateTrace [post]
func emulateTrace(c *gin.Context) {
	var req EmulateRequest
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": "invalid request"})
		return
	}

	taskID := generateTaskID()
	task := TraceTask{
		ID:  taskID,
		BOC: req.Boc,
	}

	// Serialize the task using msgpack
	var buf bytes.Buffer
	enc := msgpack.NewEncoder(&buf)
	enc.UseArrayEncodedStructs(false)

	err := enc.Encode(task)
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": "failed to serialize task"})
		return
	}

	// Initialize Redis client
	rdb := redis.NewClient(&redis.Options{
		Addr: "localhost:6379", // Redis server address
	})

	ctx := context.Background()

	// Push the packed task to the Redis queue
	if err := rdb.LPush(ctx, "somequeue", buf.Bytes()).Err(); err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": "failed to push task to Redis"})
		return
	}

	// Subscribe to the result channel
	pubsub := rdb.Subscribe(ctx, "result_channel_"+taskID)
	defer pubsub.Close()

	// Wait for the result
	msg, err := pubsub.ReceiveMessage(ctx)
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": "failed to receive result from Redis"})
		return
	}

	if msg.Payload == "error" {
		error_msg, err := rdb.Get(ctx, "error_channel_"+taskID).Result()
		if err != nil {
			c.JSON(http.StatusInternalServerError, gin.H{"error": "failed to receive error from Redis"})
			return
		}
		c.JSON(http.StatusInternalServerError, gin.H{"error": error_msg})
		return
	}
	if msg.Payload != "success" {
		c.JSON(http.StatusInternalServerError, gin.H{"error": "unexpected message from Redis"})
		return
	}

	hset, err := rdb.HGetAll(ctx, "result_hset_"+taskID).Result()
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": "failed to receive result from Redis"})
		return
	}

	result, err := models.TransformToAPIResponse(hset)
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": "failed to transform result to API response " + err.Error()})
		return
	}
	c.JSON(http.StatusOK, result)
}

// func main() {
// 	rdb := redis.NewClient(&redis.Options{
// 		Addr: "localhost:6379", // Redis server address
// 	})
// 	taskID := "ZAHyzMfcMU"

// 	ctx := context.Background()
// 	hset, err := rdb.HGetAll(ctx, "result_channel_"+taskID).Result()
// 	if err != nil {
// 		c.JSON(http.StatusInternalServerError, gin.H{"error": "failed to receive result from Redis"})
// 		return
// 	}

//     // get any value from hset
//     some_node =

// 	// root_node := hset[trace_id]

// 	var node models.TraceNode
// 	unmarshal_err := msgpack.Unmarshal([]byte(root_node), &node)
// 	if unmarshal_err != nil {
// 		c.JSON(http.StatusInternalServerError, gin.H{"error": "failed to decode result from Redis " + unmarshal_err.Error()})
// 		return
// 	}
// }

func main() {
	r := gin.Default()

	docs.SwaggerInfo.BasePath = "/api/v1"
	v1 := r.Group("/api/v1")
	{
		v1.POST("/emulateTrace", emulateTrace)
	}

	r.GET("/swagger/*any", ginSwagger.WrapHandler(swaggerfiles.Handler))
	r.Run(":8080")
}
