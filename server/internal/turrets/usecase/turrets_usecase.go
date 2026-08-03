// Package usecase holds the turrets module's business logic.
//
// M3 stub: List returns an empty slice. The full M3 — DDB-backed
// repos, registration, state ingest, command queue, robot-secret
// middleware — lands in a follow-up. The dashboard needs a 200-with-
// empty-array response to render its empty state, and that's what
// this stub provides.
package usecase

import "context"

// TurretsUseCaseInterface is the contract the HTTP layer depends on.
// Handlers take this interface, not *TurretsUseCase, so tests can
// substitute fakes.
type TurretsUseCaseInterface interface {
	List(ctx context.Context, ownerID string) (ListOutput, error)
}

// TurretsUseCase is the concrete implementation. Currently has no
// outbound dependencies — the stub returns an empty list. When M3
// proper lands, this struct will gain a TurretRepository and a
// CommandRepository.
type TurretsUseCase struct {
	// TODO(m3-proper): add repo + logger.
}

// NewTurretsUseCase wires the (currently empty) dependency graph.
func NewTurretsUseCase() *TurretsUseCase {
	return &TurretsUseCase{}
}

// List returns the turrets owned by the given user. Stub returns
// an empty list — M3 will replace this body with a GSI query.
func (uc *TurretsUseCase) List(_ context.Context, _ string) (ListOutput, error) {
	return ListOutput{Turrets: []TurretDTO{}}, nil
}

// ListOutput is the response for GET /turrets. Data is always a
// (possibly empty) slice, never null — the Flutter dashboard's
// AsyncValue<List<Turret>>.when pattern is much simpler with [].
type ListOutput struct {
	Turrets []TurretDTO `json:"turrets"`
}

// TurretDTO is the wire shape for one turret in the list. Fields
// here are what the dashboard needs to render a card; deeper state
// (telemetry, firmware, video_capture_enabled) comes in M3 proper
// with the detail screen.
type TurretDTO struct {
	RobotID            string `json:"robot_id"`
	OwnerID            string `json:"owner_id"`
	Name               string `json:"name"`
	Status             string `json:"status"`  // ONLINE | OFFLINE
	Mode               string `json:"mode"`    // MANUAL | AUTO | SAFE
	FirmwareVersion    string `json:"firmware_version,omitempty"`
	VideoCaptureEnabled bool  `json:"video_capture_enabled"`
	CreatedAt          string `json:"created_at"`
	LastSeen           string `json:"last_seen,omitempty"`
}

// Compile-time assertion that *TurretsUseCase satisfies the interface.
var _ TurretsUseCaseInterface = (*TurretsUseCase)(nil)
