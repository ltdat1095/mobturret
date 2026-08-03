package usecase_test

import (
	"context"
	"encoding/json"
	"strings"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"mobturret/server/internal/turrets/usecase"
)

func TestList_ReturnsEmptySlice(t *testing.T) {
	uc := usecase.NewTurretsUseCase()

	out, err := uc.List(context.Background(), "user-123")
	require.NoError(t, err)

	// The dashboard's AsyncValue<List<Turret>>.when pattern relies on
	// this being an empty slice, never nil — otherwise Flutter sees
	// `null` and the empty-state branch never fires.
	assert.NotNil(t, out.Turrets, "Turrets must be empty slice, not nil")
	assert.Equal(t, 0, len(out.Turrets))

	// Wire-format guarantee: serialized output must be `[]`, not `null`.
	b, err := json.Marshal(out)
	require.NoError(t, err)
	assert.True(t, strings.Contains(string(b), `"turrets":[]`),
		"serialized output must contain `\"turrets\":[]`, got %s", string(b))
}

func TestList_IgnoresOwnerForStub(t *testing.T) {
	// The stub currently doesn't filter by owner — that's M3 proper.
	// This test pins the behavior so a future change is intentional.
	uc := usecase.NewTurretsUseCase()
	_, err := uc.List(context.Background(), "any-user-id")
	require.NoError(t, err)
}
