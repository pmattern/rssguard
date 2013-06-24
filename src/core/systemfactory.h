#ifndef SYSTEMFACTORY_H
#define SYSTEMFACTORY_H


class SystemFactory {
  private:
    SystemFactory();

  public:
    enum AutoStartStatus {
      Enabled,
      Disabled,
      Unavailable
    };

    // Returns current status of auto-start function.
    static SystemFactory::AutoStartStatus getAutoStartStatus();

    // Sets new status for auto-start function.
    // Function returns false if setting of
    // new status failed.
    static bool setAutoStartStatus(const SystemFactory::AutoStartStatus &new_status);
};

#endif // SYSTEMFACTORY_H