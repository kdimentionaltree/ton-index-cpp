package models

import (
	"fmt"

	"github.com/vmihailenco/msgpack/v5"
)

type AccountStatus int

const (
	AccountStatusUninit AccountStatus = iota
	AccountStatusFrozen
	AccountStatusActive
	AccountStatusNonexist
)

type AccStatusChange int

const (
	AccStatusUnchanged AccStatusChange = iota
	AccStatusFrozen
	AccStatusDeleted
)

type ComputeSkipReason int

const (
	ComputeSkipNoState ComputeSkipReason = iota
	ComputeSkipBadState
	ComputeSkipNoGas
	ComputeSkipSuspended
)

type TrStoragePhase struct {
	StorageFeesCollected uint64          `msgpack:"storage_fees_collected" json:"storage_fees_collected"`
	StorageFeesDue       *uint64         `msgpack:"storage_fees_due" json:"storage_fees_due"`
	StatusChange         AccStatusChange `msgpack:"status_change" json:"status_change"`
}

type TrCreditPhase struct {
	DueFeesCollected *uint64 `msgpack:"due_fees_collected" json:"due_fees_collected"`
	Credit           uint64  `msgpack:"credit" json:"credit"`
}

type TrComputePhaseSkipped struct {
	Reason ComputeSkipReason `msgpack:"reason" json:"reason"`
}

type TrComputePhaseVm struct {
	Success          bool    `msgpack:"success" json:"success"`
	MsgStateUsed     bool    `msgpack:"msg_state_used" json:"msg_state_used"`
	AccountActivated bool    `msgpack:"account_activated" json:"account_activated"`
	GasFees          uint64  `msgpack:"gas_fees" json:"gas_fees"`
	GasUsed          uint64  `msgpack:"gas_used" json:"gas_used"`
	GasLimit         uint64  `msgpack:"gas_limit" json:"gas_limit"`
	GasCredit        *uint64 `msgpack:"gas_credit" json:"gas_credit"`
	Mode             int8    `msgpack:"mode" json:"mode"`
	ExitCode         int32   `msgpack:"exit_code" json:"exit_code"`
	ExitArg          *int32  `msgpack:"exit_arg" json:"exit_arg"`
	VmSteps          uint32  `msgpack:"vm_steps" json:"vm_steps"`
	VmInitStateHash  string  `msgpack:"vm_init_state_hash" json:"vm_init_state_hash"`
	VmFinalStateHash string  `msgpack:"vm_final_state_hash" json:"vm_final_state_hash"`
}

type StorageUsedShort struct {
	Cells uint64 `msgpack:"cells" json:"cells"`
	Bits  uint64 `msgpack:"bits" json:"bits"`
}

type TrActionPhase struct {
	Success         bool             `msgpack:"success" json:"success"`
	Valid           bool             `msgpack:"valid" json:"valid"`
	NoFunds         bool             `msgpack:"no_funds" json:"no_funds"`
	StatusChange    AccStatusChange  `msgpack:"status_change" json:"status_change"`
	TotalFwdFees    *uint64          `msgpack:"total_fwd_fees" json:"total_fwd_fees"`
	TotalActionFees *uint64          `msgpack:"total_action_fees" json:"total_action_fees"`
	ResultCode      int32            `msgpack:"result_code" json:"result_code"`
	ResultArg       *int32           `msgpack:"result_arg" json:"result_arg"`
	TotActions      uint16           `msgpack:"tot_actions" json:"tot_actions"`
	SpecActions     uint16           `msgpack:"spec_actions" json:"spec_actions"`
	SkippedActions  uint16           `msgpack:"skipped_actions" json:"skipped_actions"`
	MsgsCreated     uint16           `msgpack:"msgs_created" json:"msgs_created"`
	ActionListHash  string           `msgpack:"action_list_hash" json:"action_list_hash"`
	TotMsgSize      StorageUsedShort `msgpack:"tot_msg_size" json:"tot_msg_size"`
}

type TrBouncePhaseNegfunds struct {
	Dummy bool `msgpack:"dummy" json:"dummy"`
}

type TrBouncePhaseNofunds struct {
	MsgSize    StorageUsedShort `msgpack:"msg_size" json:"msg_size"`
	ReqFwdFees uint64           `msgpack:"req_fwd_fees" json:"req_fwd_fees"`
}

type TrBouncePhaseOk struct {
	MsgSize StorageUsedShort `msgpack:"msg_size" json:"msg_size"`
	MsgFees uint64           `msgpack:"msg_fees" json:"msg_fees"`
	FwdFees uint64           `msgpack:"fwd_fees" json:"fwd_fees"`
}

type Message struct {
	Hash         string  `msgpack:"hash" json:"hash"`
	Source       *string `msgpack:"source" json:"source"`
	Destination  *string `msgpack:"destination" json:"destination"`
	Value        *uint64 `msgpack:"value" json:"value"`
	FwdFee       *uint64 `msgpack:"fwd_fee" json:"fwd_fee"`
	IhrFee       *uint64 `msgpack:"ihr_fee" json:"ihr_fee"`
	CreatedLt    *uint64 `msgpack:"created_lt" json:"created_lt"`
	CreatedAt    *uint32 `msgpack:"created_at" json:"created_at"`
	Opcode       *int32  `msgpack:"opcode" json:"opcode"`
	IhrDisabled  *bool   `msgpack:"ihr_disabled" json:"ihr_disabled"`
	Bounce       *bool   `msgpack:"bounce" json:"bounce"`
	Bounced      *bool   `msgpack:"bounced" json:"bounced"`
	ImportFee    *uint64 `msgpack:"import_fee" json:"import_fee"`
	BodyBoc      string  `msgpack:"body_boc" json:"body_boc"`
	InitStateBoc *string `msgpack:"init_state_boc" json:"init_state_boc"`
}

type TransactionDescr struct {
	CreditFirst bool            `msgpack:"credit_first" json:"credit_first"`
	StoragePh   TrStoragePhase  `msgpack:"storage_ph" json:"storage_ph"`
	CreditPh    TrCreditPhase   `msgpack:"credit_ph" json:"credit_ph"`
	ComputePh   ComputePhaseVar `msgpack:"compute_ph" json:"compute_ph"`
	Action      *TrActionPhase  `msgpack:"action" json:"action"`
	Aborted     bool            `msgpack:"aborted" json:"aborted"`
	Bounce      *BouncePhaseVar `msgpack:"bounce" json:"bounce"`
	Destroyed   bool            `msgpack:"destroyed" json:"destroyed"`
}

type Transaction struct {
	Hash                   string           `msgpack:"hash" json:"hash"`
	Account                string           `msgpack:"account" json:"account"`
	Lt                     uint64           `msgpack:"lt" json:"lt,string"`
	PrevTransHash          string           `msgpack:"prev_trans_hash" json:"prev_trans_hash"`
	PrevTransLt            uint64           `msgpack:"prev_trans_lt" json:"prev_trans_lt,string"`
	Now                    uint32           `msgpack:"now" json:"now"`
	OrigStatus             AccountStatus    `msgpack:"orig_status" json:"orig_status"`
	EndStatus              AccountStatus    `msgpack:"end_status" json:"end_status"`
	InMsg                  *Message         `msgpack:"in_msg" json:"in_msg"`
	OutMsgs                []Message        `msgpack:"out_msgs" json:"out_msgs"`
	TotalFees              uint64           `msgpack:"total_fees" json:"total_fees"`
	AccountStateHashBefore string           `msgpack:"account_state_hash_before" json:"account_state_hash_before"`
	AccountStateHashAfter  string           `msgpack:"account_state_hash_after" json:"account_state_hash_after"`
	Description            TransactionDescr `msgpack:"description" json:"description"`
}

type TraceNode struct {
	Transaction Transaction `msgpack:"transaction"`
	Emulated    bool        `msgpack:"emulated"`
}

type ComputePhaseVar struct {
	Type uint8
	Data interface{} // Can be TrComputePhaseSkipped or TrComputePhaseVm
}

var _ msgpack.CustomDecoder = (*ComputePhaseVar)(nil)

func (s *ComputePhaseVar) DecodeMsgpack(dec *msgpack.Decoder) error {
	length, err := dec.DecodeArrayLen()
	if err != nil {
		return err
	}
	if length != 2 {
		return fmt.Errorf("invalid variant array length: %d", length)
	}

	index, err := dec.DecodeUint8()
	if err != nil {
		return err
	}

	switch index {
	case 0:
		var a TrComputePhaseSkipped
		err = dec.Decode(&a)
		s.Data = a
	case 1:
		var b TrComputePhaseVm
		err = dec.Decode(&b)
		s.Data = b
	default:
		return fmt.Errorf("unknown variant index: %d", index)
	}

	s.Type = index
	return err
}

type BouncePhaseVar struct {
	Type uint8
	Data interface{} // Can be TrBouncePhaseNegfunds, TrBouncePhaseNofunds or TrBouncePhaseOk
}

var _ msgpack.CustomDecoder = (*BouncePhaseVar)(nil)

func (s *BouncePhaseVar) DecodeMsgpack(dec *msgpack.Decoder) error {
	length, err := dec.DecodeArrayLen()
	if err != nil {
		return err
	}
	if length != 2 {
		return fmt.Errorf("invalid variant array length: %d", length)
	}

	fmt.Println(length)

	index, err := dec.DecodeUint8()
	if err != nil {
		return err
	}

	switch index {
	case 0:
		var a TrBouncePhaseNegfunds
		err = dec.Decode(&a)
		s.Data = a
	case 1:
		var b TrBouncePhaseNofunds
		err = dec.Decode(&b)
		s.Data = b
	case 2:
		var c TrBouncePhaseOk
		err = dec.Decode(&c)
		s.Data = c
	default:
		return fmt.Errorf("unknown variant index: %d", index)
	}

	s.Type = index
	return err
}
