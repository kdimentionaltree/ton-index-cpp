package models

import tonindexgo "github.com/kdimentionaltree/ton-index-go/index"

type TraceNodeShort struct {
	TransactionHash Hash              `json:"tx_hash"`
	InMsgHash       Hash              `json:"in_msg_hash"`
	Children        []*TraceNodeShort `json:"children"`
}

type EmulateTraceResponse struct {
	McBlockID     string                 `json:"mc_block_id"`
	Trace         TraceNodeShort         `json:"trace"`
	Transactions  map[Hash]*Transaction  `json:"transactions"`
	AccountStates map[Hash]*AccountState `json:"account_states"`
	Actions       *[]tonindexgo.Action   `json:"actions,omitempty"`
}
