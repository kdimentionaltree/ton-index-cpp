package models

type TraceNodeShort struct {
	TransactionHash string            `json:"tx_hash"`
	InMsgHash       string            `json:"in_msg_hash"`
	Children        []*TraceNodeShort `json:"children"`
	// AccountStateBefore string `json:"account_state_before"`
	// AccountStateBefore string `json:"account_state_before"`
}

type EmulateTraceResponse struct {
	McBlockID    string                  `json:"mc_block_id"`
	Trace        TraceNodeShort          `json:"trace"`
	Transactions map[string]*Transaction `json:"transactions"`
	// AccountStates map[string]*AccountState    `json:"account_states"`
}
