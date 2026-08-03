// Package main is the MobTurret HTTP server entry point.
//
// Lifecycle:
//  1. Load config from .env + env vars (Viper).
//  2. Build slog logger; install as slog.SetDefault.
//  3. Build DynamoDB client; ensure tables exist (idempotent).
//  4. Construct repositories, usecase, handlers.
//  5. Build gin engine, register routes, apply middleware.
//  6. Start http.Server. Block until SIGINT/SIGTERM, then graceful shutdown.
//
// Why manual DI (no Wire)? This server has a single auth module with
// six providers. Wire's compile-time DI shines when the graph has
// 20+ providers and deep interface bindings; for this MVP, a
// straight-line constructor chain in main() is clearer. If a second
// domain module lands, we revisit.
package main

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/aws/aws-sdk-go-v2/service/dynamodb"
	"github.com/gin-gonic/gin"

	authhandler "mobturret/server/internal/auth/adapter/handler"
	authrepo "mobturret/server/internal/auth/adapter/repository"
	"mobturret/server/internal/auth/usecase"
	"mobturret/server/internal/platform/config"
	"mobturret/server/internal/platform/database"
	platformlogger "mobturret/server/internal/platform/logger"
	"mobturret/server/internal/platform/middleware"
	"mobturret/server/internal/platform/security"
	turrethandler "mobturret/server/internal/turrets/adapter/handler"
	turretusecase "mobturret/server/internal/turrets/usecase"
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintf(os.Stderr, "fatal: %v\n", err)
		os.Exit(1)
	}
}

func run() error {
	// 1. Config.
	cfg, err := config.LoadConfig("")
	if err != nil {
		return fmt.Errorf("load config: %w", err)
	}

	// 2. Logger.
	log := platformlogger.New(cfg.Log)
	slog.SetDefault(log)

	log.Info("starting mobturret-server",
		slog.String("env", cfg.Server.Env),
		slog.String("port", cfg.Server.Port),
		slog.String("ddb_endpoint", cfg.DDB.Endpoint),
		slog.String("users_table", cfg.DDB.UsersTable),
	)

	// 3. DynamoDB client + table provisioning.
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	ddb, err := database.NewDynamoDBClient(ctx, cfg)
	if err != nil {
		return fmt.Errorf("dynamodb client: %w", err)
	}

	if err := database.EnsureTablesExist(ctx, ddb, []database.TableSpec{
		database.UsersTable(cfg.DDB.UsersTable),
	}); err != nil {
		return fmt.Errorf("ensure tables: %w", err)
	}
	log.Info("dynamodb tables ready")

	// 4. Domain layer wiring.
	users := authrepo.NewDynamoUserRepo(ddb, cfg.DDB.UsersTable)
	tokens := security.New(
		[]byte(cfg.JWT.SecretKey),
		cfg.JWT.Issuer,
		cfg.JWT.Audience,
		cfg.JWT.TTLHours,
	)
	authUC := usecase.NewAuthUseCase(users, tokens, log)
	authH := authhandler.NewAuthHandler(authUC, log)
	jwtMW := middleware.NewJWTMiddleware(tokens, log)

	// Turrets module — M3 stub. No repository yet; the usecase
	// returns a hard-coded empty list. The GSI-backed list query
	// lands with M3 proper.
	turretUC := turretusecase.NewTurretsUseCase()
	turretH := turrethandler.NewTurretHandler(turretUC, log)

	// 5. Gin engine.
	engine := buildEngine(cfg, log, authH, jwtMW, turretH)

	// 6. HTTP server with graceful shutdown.
	srv := &http.Server{
		Addr:              ":" + cfg.Server.Port,
		Handler:           engine,
		ReadHeaderTimeout: 10 * time.Second,
	}

	serverErr := make(chan error, 1)
	go func() {
		log.Info("listening", slog.String("addr", srv.Addr))
		if err := srv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			serverErr <- err
		}
		close(serverErr)
	}()

	quit := make(chan os.Signal, 1)
	signal.Notify(quit, syscall.SIGINT, syscall.SIGTERM)

	select {
	case sig := <-quit:
		log.Info("shutdown signal received", slog.String("signal", sig.String()))
	case err := <-serverErr:
		if err != nil {
			return fmt.Errorf("listen: %w", err)
		}
	}

	shutdownCtx, shutdownCancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer shutdownCancel()
	if err := srv.Shutdown(shutdownCtx); err != nil {
		return fmt.Errorf("graceful shutdown: %w", err)
	}
	log.Info("shutdown complete")
	return nil
}

// buildEngine wires gin routes + middleware.
//
// Middleware order (outer → inner):
//  1. RequestID — must come first so all logs (including recovery)
//     are tagged with the request id.
//  2. Logger (gin.Default()) — gin's built-in access log.
//  3. Recovery (gin.Default()) — panics become 500.
//  4. CORS — last so preflight requests get the headers.
func buildEngine(
	cfg *config.Config,
	log *slog.Logger,
	authH *authhandler.AuthHandler,
	jwtMW *middleware.JWTMiddleware,
	turretH *turrethandler.TurretHandler,
) *gin.Engine {
	if cfg.Server.Env == "production" {
		gin.SetMode(gin.ReleaseMode)
	}

	engine := gin.New()
	engine.Use(
		middleware.RequestID(),
		gin.Logger(),
		gin.Recovery(),
		middleware.CORS(),
	)

	// Health check — no auth.
	engine.GET("/healthz", func(c *gin.Context) {
		c.JSON(http.StatusOK, gin.H{"status": "ok"})
	})

	// Self-describing root — so a browser hitting `http://host:port/`
	// gets something useful instead of Gin's bare 404. Lists the
	// exposed routes; the Flutter app never calls this.
	engine.GET("/", func(c *gin.Context) {
		c.JSON(http.StatusOK, gin.H{
			"service": "mobturret-server",
			"env":     cfg.Server.Env,
			"routes": gin.H{
				"public":    []string{"GET /healthz", "POST /auth/signup", "POST /auth/login"},
				"protected": []string{"GET /auth/me", "GET /turrets"},
			},
			"docs": "see /PLAN.md in the repo",
		})
	})

	// Auth routes.
	authH.RegisterPublic(engine)

	// Protected group: every route here requires a valid JWT.
	protected := engine.Group("")
	protected.Use(jwtMW.Authenticate())
	authH.RegisterProtected(protected)
	turretH.RegisterProtected(protected)

	return engine
}

// Compile-time check that dynamodb.Client is what we expect; this
// makes the build fail loud if aws-sdk-go-v2 bumps the API.
var _ *dynamodb.Client = nil
