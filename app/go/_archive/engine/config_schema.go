package engine

// ScaleFX Engine - Config Schema Validation DSL
// Lightweight YAML-subset validator that mirrors the C++ declarative schema
// (prop<>, group<>, seq<>) for client-side config validation before upload.
//
// This is NOT a full YAML parser — it validates structure, key presence,
// and value types/ranges against a declared schema. The firmware does the
// real parsing; this catches obvious errors early on the Go side.

import (
	"fmt"
	"strconv"
	"strings"
)

// ─── Schema Node Types ───

// FieldType describes the expected YAML scalar type.
type FieldType int

const (
	FieldString FieldType = iota // Free-form string
	FieldBool                    // true/false/yes/no
	FieldInt                     // Integer (with optional range)
	FieldFloat                   // Float
	FieldEnum                    // One of a fixed set of strings
)

// Field describes a single YAML scalar property.
type Field struct {
	Key      string    // YAML key name
	Type     FieldType // Expected type
	Required bool      // Must be present (default: false — all fields optional per C++ pattern)
	Min      *float64  // Range minimum (FieldInt/FieldFloat only)
	Max      *float64  // Range maximum (FieldInt/FieldFloat only)
	Choices  []string  // Allowed values (FieldEnum only)
}

// Group describes a YAML map child (mirrors C++ group<>).
type Group struct {
	Key      string  // YAML key name
	Required bool    // Must be present
	Schema   *Schema // Child schema
}

// Sequence describes a YAML sequence of maps (mirrors C++ seq<>).
type Sequence struct {
	Key      string  // YAML key name
	Required bool    // Must be present
	MaxItems int     // Maximum number of items (0 = unlimited)
	Schema   *Schema // Schema for each sequence item
}

// Schema is a collection of field, group, and sequence definitions.
type Schema struct {
	Name       string     // Display name (e.g., "GearControl", "HubFX")
	Fields     []Field    // Scalar properties
	Groups     []Group    // Map children
	Sequences  []Sequence // Sequence children
}

// ─── Schema Builder DSL ───

// NewSchema creates a named schema.
func NewSchema(name string) *Schema {
	return &Schema{Name: name}
}

// Prop adds a scalar field to the schema.
func (s *Schema) Prop(key string, ft FieldType) *Schema {
	s.Fields = append(s.Fields, Field{Key: key, Type: ft})
	return s
}

// PropRequired adds a required scalar field.
func (s *Schema) PropRequired(key string, ft FieldType) *Schema {
	s.Fields = append(s.Fields, Field{Key: key, Type: ft, Required: true})
	return s
}

// PropRange adds an integer/float field with range validation.
func (s *Schema) PropRange(key string, ft FieldType, min, max float64) *Schema {
	s.Fields = append(s.Fields, Field{Key: key, Type: ft, Min: &min, Max: &max})
	return s
}

// PropEnum adds a string field with allowed values.
func (s *Schema) PropEnum(key string, choices ...string) *Schema {
	s.Fields = append(s.Fields, Field{Key: key, Type: FieldEnum, Choices: choices})
	return s
}

// SubGroup adds a map child group.
func (s *Schema) SubGroup(key string, child *Schema) *Schema {
	s.Groups = append(s.Groups, Group{Key: key, Schema: child})
	return s
}

// SubGroupRequired adds a required map child group.
func (s *Schema) SubGroupRequired(key string, child *Schema) *Schema {
	s.Groups = append(s.Groups, Group{Key: key, Required: true, Schema: child})
	return s
}

// Seq adds a sequence of maps.
func (s *Schema) Seq(key string, maxItems int, itemSchema *Schema) *Schema {
	s.Sequences = append(s.Sequences, Sequence{Key: key, MaxItems: maxItems, Schema: itemSchema})
	return s
}

// SeqRequired adds a required sequence.
func (s *Schema) SeqRequired(key string, maxItems int, itemSchema *Schema) *Schema {
	s.Sequences = append(s.Sequences, Sequence{Key: key, Required: true, MaxItems: maxItems, Schema: itemSchema})
	return s
}

// ─── Minimal YAML Parser ───
// Parses the YAML subset used by ScaleFX configs into a tree of yamlNode.

type yamlNodeType int

const (
	yamlMap yamlNodeType = iota
	yamlSequence
	yamlScalar
)

type yamlNode struct {
	nodeType yamlNodeType
	key      string
	value    string      // scalar value
	children []*yamlNode // map children or sequence items
	line     int         // source line number (1-based)
}

// child finds a direct child by key.
func (n *yamlNode) child(key string) *yamlNode {
	if n == nil {
		return nil
	}
	for _, c := range n.children {
		if c.key == key {
			return c
		}
	}
	return nil
}

type yamlLine struct {
	indent int
	key    string
	value  string
	isList bool // starts with "- "
	lineNo int
}

// parseYAML parses a minimal YAML subset into a tree.
func parseYAML(input string) (*yamlNode, []string) {
	var errors []string
	lines := tokenizeYAML(input)
	root := &yamlNode{nodeType: yamlMap, line: 1}
	if len(lines) == 0 {
		return root, errors
	}
	_, errs := buildTree(root, lines, 0, 0)
	errors = append(errors, errs...)
	return root, errors
}

func tokenizeYAML(input string) []yamlLine {
	raw := strings.Split(input, "\n")
	var lines []yamlLine
	for i, line := range raw {
		// Strip comments (only full-line or trailing after whitespace)
		if idx := strings.Index(line, " #"); idx >= 0 {
			line = line[:idx]
		}
		trimmed := strings.TrimSpace(line)
		if trimmed == "" || trimmed[0] == '#' {
			continue
		}
		indent := 0
		for _, ch := range line {
			if ch == ' ' {
				indent++
			} else {
				break
			}
		}
		isList := strings.HasPrefix(trimmed, "- ")
		content := trimmed
		if isList {
			content = strings.TrimPrefix(trimmed, "- ")
			indent += 2 // normalize: list item content is at indent+2
		}

		key, value := "", ""
		if colonIdx := strings.Index(content, ": "); colonIdx >= 0 {
			key = strings.TrimSpace(content[:colonIdx])
			value = strings.TrimSpace(content[colonIdx+2:])
		} else if strings.HasSuffix(content, ":") {
			key = strings.TrimSuffix(content, ":")
		} else if isList {
			// Bare list item scalar (e.g., "- off")
			value = content
		} else {
			key = content
		}
		// Strip quotes from values
		value = stripQuotes(value)

		lines = append(lines, yamlLine{
			indent: indent,
			key:    key,
			value:  value,
			isList: isList,
			lineNo: i + 1,
		})
	}
	return lines
}

func stripQuotes(s string) string {
	if len(s) >= 2 && ((s[0] == '"' && s[len(s)-1] == '"') || (s[0] == '\'' && s[len(s)-1] == '\'')) {
		return s[1 : len(s)-1]
	}
	return s
}

// buildTree recursively builds the YAML node tree. Returns the number of lines consumed.
func buildTree(parent *yamlNode, lines []yamlLine, startIdx int, baseIndent int) (int, []string) {
	var errors []string
	i := startIdx
	for i < len(lines) {
		ln := lines[i]
		if ln.indent < baseIndent {
			break // Dedent — return to parent
		}

		if ln.isList {
			// Find or create the sequence node at the current parent level
			// Lists are typically keyed by the previous map entry
			// For our purposes, list items become children of parent directly
			itemNode := &yamlNode{nodeType: yamlMap, line: ln.lineNo}
			if ln.key != "" {
				// "- key: value" — first entry of a multi-key map item
				child := &yamlNode{
					nodeType: yamlScalar,
					key:      ln.key,
					value:    ln.value,
					line:     ln.lineNo,
				}
				itemNode.children = append(itemNode.children, child)
			} else if ln.value != "" {
				// "- scalar" — bare scalar in a sequence
				itemNode.nodeType = yamlScalar
				itemNode.value = ln.value
			}
			i++

			// Read continuation lines at deeper indent
			if i < len(lines) && lines[i].indent > ln.indent {
				consumed, errs := buildTree(itemNode, lines, i, lines[i].indent)
				errors = append(errors, errs...)
				i += consumed
			}
			parent.children = append(parent.children, itemNode)
			continue
		}

		if ln.value != "" {
			// key: value — scalar
			node := &yamlNode{
				nodeType: yamlScalar,
				key:      ln.key,
				value:    ln.value,
				line:     ln.lineNo,
			}
			parent.children = append(parent.children, node)
			i++
		} else if ln.key != "" {
			// key: — map or sequence container
			node := &yamlNode{key: ln.key, line: ln.lineNo}
			i++
			if i < len(lines) && lines[i].indent > ln.indent {
				// Check if the next line starts a list
				if lines[i].isList {
					node.nodeType = yamlSequence
				} else {
					node.nodeType = yamlMap
				}
				consumed, errs := buildTree(node, lines, i, lines[i].indent)
				errors = append(errors, errs...)
				i += consumed
			} else {
				node.nodeType = yamlScalar
				node.value = ""
			}
			parent.children = append(parent.children, node)
		} else {
			i++
		}
	}
	return i - startIdx, errors
}

// ─── Schema Validation ───

// Validate parses the YAML string and validates it against this schema.
// Returns a list of human-readable error strings (empty = valid).
func (s *Schema) Validate(yamlText string) []string {
	root, parseErrors := parseYAML(yamlText)
	if len(parseErrors) > 0 {
		return parseErrors
	}
	return s.validateNode(root, "")
}

func (s *Schema) validateNode(node *yamlNode, path string) []string {
	var errors []string

	// Check required fields
	for _, f := range s.Fields {
		child := node.child(f.Key)
		if child == nil {
			if f.Required {
				errors = append(errors, fmt.Sprintf("%s: missing required field '%s'", pathStr(path), f.Key))
			}
			continue
		}
		errors = append(errors, validateField(f, child, joinPath(path, f.Key))...)
	}

	// Check required groups
	for _, g := range s.Groups {
		child := node.child(g.Key)
		if child == nil {
			if g.Required {
				errors = append(errors, fmt.Sprintf("%s: missing required group '%s'", pathStr(path), g.Key))
			}
			continue
		}
		if child.nodeType != yamlMap {
			errors = append(errors, fmt.Sprintf("%s: expected map, got scalar", joinPath(path, g.Key)))
			continue
		}
		errors = append(errors, g.Schema.validateNode(child, joinPath(path, g.Key))...)
	}

	// Check sequences
	for _, seq := range s.Sequences {
		child := node.child(seq.Key)
		if child == nil {
			if seq.Required {
				errors = append(errors, fmt.Sprintf("%s: missing required sequence '%s'", pathStr(path), seq.Key))
			}
			continue
		}
		if child.nodeType != yamlSequence {
			errors = append(errors, fmt.Sprintf("%s: expected sequence, got %s", joinPath(path, seq.Key), nodeTypeName(child.nodeType)))
			continue
		}
		if seq.MaxItems > 0 && len(child.children) > seq.MaxItems {
			errors = append(errors, fmt.Sprintf("%s: too many items (%d, max %d)", joinPath(path, seq.Key), len(child.children), seq.MaxItems))
		}
		for idx, item := range child.children {
			itemPath := fmt.Sprintf("%s[%d]", joinPath(path, seq.Key), idx)
			if item.nodeType != yamlMap && item.nodeType != yamlScalar {
				errors = append(errors, fmt.Sprintf("%s: expected map item", itemPath))
				continue
			}
			errors = append(errors, seq.Schema.validateNode(item, itemPath)...)
		}
	}

	// Warn about unknown top-level keys
	known := s.knownKeys()
	for _, child := range node.children {
		if child.key != "" && !known[child.key] {
			errors = append(errors, fmt.Sprintf("%s: unknown key '%s' (line %d)", pathStr(path), child.key, child.line))
		}
	}

	return errors
}

func (s *Schema) knownKeys() map[string]bool {
	m := make(map[string]bool)
	for _, f := range s.Fields {
		m[f.Key] = true
	}
	for _, g := range s.Groups {
		m[g.Key] = true
	}
	for _, seq := range s.Sequences {
		m[seq.Key] = true
	}
	return m
}

func validateField(f Field, node *yamlNode, path string) []string {
	var errors []string
	val := node.value

	switch f.Type {
	case FieldBool:
		lower := strings.ToLower(val)
		if lower != "true" && lower != "false" && lower != "yes" && lower != "no" {
			errors = append(errors, fmt.Sprintf("%s: expected bool, got '%s'", path, val))
		}

	case FieldInt:
		v, err := strconv.ParseInt(val, 10, 64)
		if err != nil {
			errors = append(errors, fmt.Sprintf("%s: expected integer, got '%s'", path, val))
		} else {
			if f.Min != nil && float64(v) < *f.Min {
				errors = append(errors, fmt.Sprintf("%s: value %d below minimum %.0f", path, v, *f.Min))
			}
			if f.Max != nil && float64(v) > *f.Max {
				errors = append(errors, fmt.Sprintf("%s: value %d above maximum %.0f", path, v, *f.Max))
			}
		}

	case FieldFloat:
		v, err := strconv.ParseFloat(val, 64)
		if err != nil {
			errors = append(errors, fmt.Sprintf("%s: expected number, got '%s'", path, val))
		} else {
			if f.Min != nil && v < *f.Min {
				errors = append(errors, fmt.Sprintf("%s: value %g below minimum %g", path, v, *f.Min))
			}
			if f.Max != nil && v > *f.Max {
				errors = append(errors, fmt.Sprintf("%s: value %g above maximum %g", path, v, *f.Max))
			}
		}

	case FieldEnum:
		found := false
		lower := strings.ToLower(val)
		for _, c := range f.Choices {
			if lower == strings.ToLower(c) {
				found = true
				break
			}
		}
		if !found {
			errors = append(errors, fmt.Sprintf("%s: invalid value '%s' (expected one of: %s)", path, val, strings.Join(f.Choices, ", ")))
		}

	case FieldString:
		// No validation for free-form strings
	}

	return errors
}

func joinPath(base, key string) string {
	if base == "" {
		return key
	}
	return base + "." + key
}

func pathStr(path string) string {
	if path == "" {
		return "(root)"
	}
	return path
}

func nodeTypeName(t yamlNodeType) string {
	switch t {
	case yamlMap:
		return "map"
	case yamlSequence:
		return "sequence"
	case yamlScalar:
		return "scalar"
	}
	return "unknown"
}
