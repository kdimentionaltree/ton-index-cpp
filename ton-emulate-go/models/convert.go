package models

import (
	"encoding/base64"
	"encoding/json"
	"fmt"
	"reflect"
	"strings"

	tonindexgo "github.com/kdimentionaltree/ton-index-go/index"
	msgpack "github.com/vmihailenco/msgpack/v5"
)

func ConvertHsetToTraceNodeShort(hset map[string]string) (*TraceNodeShort, map[Hash]*Transaction, error) {
	trace_id := hset["root_node"]
	rootNodeBytes := []byte(hset[trace_id])
	if len(rootNodeBytes) == 0 {
		return nil, nil, fmt.Errorf("root node not found")
	}

	txMap := make(map[Hash]*Transaction)
	node, err := convertNode(hset, rootNodeBytes, txMap)
	if err != nil {
		return nil, nil, fmt.Errorf("failed to convert root node: %w", err)
	}
	return node, txMap, nil
}

func convertNode(hset map[string]string, nodeBytes []byte, txMap map[Hash]*Transaction) (*TraceNodeShort, error) {
	var node TraceNode
	err := msgpack.Unmarshal(nodeBytes, &node)
	if err != nil {
		return nil, fmt.Errorf("failed to unmarshal node: %w", err)
	}

	txMap[node.Transaction.Hash] = &node.Transaction

	short := &TraceNodeShort{
		TransactionHash: node.Transaction.Hash,
		InMsgHash:       node.Transaction.InMsg.Hash,
		Children:        make([]*TraceNodeShort, 0, len(node.Transaction.OutMsgs)),
	}

	for _, outMsg := range node.Transaction.OutMsgs {
		// Get child node by out message hash
		msgKey := base64.StdEncoding.EncodeToString(outMsg.Hash[:])
		if childBytes, exists := hset[msgKey]; exists {
			childNode := []byte(childBytes)
			nodeShort, err := convertNode(hset, childNode, txMap)
			if err != nil {
				return nil, fmt.Errorf("failed to convert child node: %w", err)
			}
			short.Children = append(short.Children, nodeShort)
		}
	}

	return short, nil
}

func TransformToAPIResponse(hset map[string]string) (*EmulateTraceResponse, error) {
	shortTrace, txMap, err := ConvertHsetToTraceNodeShort(hset)
	if err != nil {
		return nil, err
	}

	var accountStates map[Hash]*AccountState
	err = msgpack.Unmarshal([]byte(hset["account_states"]), &accountStates)
	if err != nil {
		return nil, fmt.Errorf("failed to unmarshal account states: %w", err)
	}

	var actionsPointer *[]tonindexgo.Action = nil
	actionsData, ok := hset["actions"]
	if ok {
		var actions []map[string]interface{}
		err = msgpack.Unmarshal([]byte(actionsData), &actions)
		if err != nil {
			return nil, fmt.Errorf("failed to unmarshal actions: %w", err)
		}

		goActions := make([]tonindexgo.Action, 0, len(actions))
		for _, rawActionMap := range actions {
			var rawAction tonindexgo.RawAction
			err := actionMapToStruct(rawActionMap, &rawAction)
			if err != nil {
				return nil, fmt.Errorf("failed to convert action: %w", err)
			}

			goAction, err := tonindexgo.ParseRawAction(&rawAction)
			if err != nil {
				return nil, fmt.Errorf("failed to parse action: %w", err)
			}
			goActions = append(goActions, *goAction)
		}
		actionsPointer = &goActions
	}

	response := EmulateTraceResponse{
		McBlockID:     hset["mc_block_id"],
		Trace:         *shortTrace,
		Transactions:  txMap,
		AccountStates: accountStates,
		Actions:       actionsPointer,
	}
	return &response, nil
}

func actionMapToStruct(input map[string]interface{}, output interface{}) error {
	flattenedMap := make(map[string]interface{})
	prepareActionMap("", input, flattenedMap)

	data, err := json.Marshal(flattenedMap)
	if err != nil {
		return fmt.Errorf("failed to marshal map to json: %w", err)
	}
	json.Unmarshal(data, output)
	return nil
}

func prepareActionMap(prefix string, src map[string]interface{}, dest map[string]interface{}) {
	for k, v := range src {
		compoundKey := k
		if prefix != "" {
			compoundKey = prefix + "_" + k
		}
		compoundKey = strings.ReplaceAll(compoundKey, "_data", "")

		compoundKey = strings.ReplaceAll(compoundKey, "_", " ")
		compoundKey = strings.Title(compoundKey)
		compoundKey = strings.ReplaceAll(compoundKey, " ", "")

		if reflect.ValueOf(v).Kind() == reflect.Map {
			submap, ok := v.(map[string]interface{})
			if ok {
				prepareActionMap(compoundKey, submap, dest)
			}
			continue
		}

		dest[compoundKey] = v
	}
}
