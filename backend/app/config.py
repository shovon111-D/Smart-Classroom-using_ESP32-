from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    supabase_url: str = ""
    supabase_service_key: str = ""
    smtp_host: str = "smtp.gmail.com"
    smtp_port: int = 587
    smtp_user: str = ""
    smtp_app_password: str = ""
    alert_email_from: str = ""
    alert_email_to: str = ""
    door_unlock_passkey: str = ""
    door_lock_passkey: str = ""
    cors_origins: str = "http://localhost:5173,http://127.0.0.1:5173"
    local_tz: str = "UTC"
    offline_after_s: int = 15
    registration_timeout_s: int = 30
    command_ttl_s: int = 30
    severity_medium_min: int = 2200
    severity_high_min: int = 3000
    sensor_retention_hours: int = 24

    model_config = SettingsConfigDict(env_file=".env", env_ignore_empty=True, extra="ignore")

    @property
    def allowed_origins(self) -> list[str]:
        return [origin.strip() for origin in self.cors_origins.split(",") if origin.strip()]

settings = Settings()
