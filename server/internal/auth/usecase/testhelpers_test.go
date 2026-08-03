package usecase_test

import "encoding/json"

// jsonMarshal is a tiny helper so the auth_usecase_test.go can scan
// response envelopes without pulling encoding/json into every test.
func jsonMarshal(v any) ([]byte, error) {
	return json.Marshal(v)
}
