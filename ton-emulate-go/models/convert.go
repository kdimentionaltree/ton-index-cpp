package models

import (
	"encoding/base64"
	"fmt"

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
	err2 := msgpack.Unmarshal([]byte(hset["account_states"]), &accountStates)
	if err2 != nil {
		return nil, fmt.Errorf("failed to unmarshal account states: %w", err2)
	}

	response := EmulateTraceResponse{
		McBlockID:     hset["mc_block_id"],
		Trace:         *shortTrace,
		Transactions:  txMap,
		AccountStates: accountStates,
	}
	return &response, nil
}
