import React, { FC, useState } from 'react';
import { YBLoading } from '../../common/indicators';
import { HAReplicationError } from './HAReplicationError';
import { HAReplicationForm } from './HAReplicationForm';
import { HAReplicationView } from './HAReplicationView';
import { useLoadHAConfiguration } from '../hooks/useLoadHAConfiguration';

export const HAReplication: FC = () => {
  const [isEditingConfig, setEditingConfig] = useState(false);
  const { config, schedule, error, isNoHAConfigExists, isLoading } = useLoadHAConfiguration(true);

  const editConfig = () => setEditingConfig(true);
  const backToViewMode = () => setEditingConfig(false);

  if (isLoading) {
    return <YBLoading />;
  }

  if (error) {
    return <HAReplicationError error={error} />;
  }

  if (isNoHAConfigExists) {
    return <HAReplicationForm backToViewMode={backToViewMode} />;
  }

  if (config && schedule) {
    if (isEditingConfig) {
      return (
        <HAReplicationForm
          config={config}
          schedule={schedule}
          backToViewMode={backToViewMode}
        />
      );
    } else {
      return (
        <HAReplicationView
          config={config}
          schedule={schedule}
          editConfig={editConfig}
        />
      );
    }
  }

  // return <HAReplicationForm backToViewMode={backToViewMode} />;

  return null; // should never get here
};
