import {YBButton, YBCheckBox} from "../../../common/forms/fields";
import React, {useEffect, useRef, useState} from "react";
import {DropdownButton, MenuItem} from "react-bootstrap";
import moment from 'moment';

import {getFeatureState} from "../../../../utils/LayoutUtils";

const filterTypes = [
  { label: 'Last 24 hrs', type: 'days', value: '1' },
  { label: 'Last 3 days', type: 'days', value: '3' },
  { label: 'Last 7 days', type: 'days', value: '7' },
  { type: 'divider' },
  { label: 'Custom', type: 'custom', value: 'custom' }
];
const selectionOptions = [
  { label: 'All', value: 'All' },
  { label: 'Application logs', value: 'ApplicationLogs' },
  { label: 'Universe logs', value: 'UniverseLogs' },
  { label: 'Output files', value: 'OutputFiles' },
  { label: 'Error files', value: 'ErrorFiles' },
  { label: 'G-Flag configurations', value: 'GFlags' },
  { label: 'Instance files', value: 'Instance' },
  { label: 'Consensus meta files', value: 'ConsensusMeta' },
  { label: 'Tablet meta files', value: 'TabletMeta' },
];

const getBackDateByDay = (day) => {
  return new Date(new Date().setDate(new Date().getDate() - day));
}


export const SecondStep = ({ onOptionsChange }) => {
  const [selectedFilterType, setSelectedFilterType] = useState(filterTypes[0].value);
  const [selectionOptionsValue, setSelectionOptionsValue] = useState(selectionOptions.map(()=> true));
  const refs = useRef([]);

  const updateOptions = (dateType) => {
    let startDate = new moment(new Date());
    let endDate = new moment(new Date());

    if(dateType !== 'custom') {
      startDate = new moment(getBackDateByDay(+dateType));
    }
    const components = [];
    selectionOptionsValue.forEach((selectionOption, index) => {
      if(index !== 0 && selectionOption) {
        components.push(selectionOptions[index].value);
      }
    })
    const payload = {
      startDate: startDate.format('yyyy-MM-DD'),
      endDate: endDate.format('yyyy-MM-DD'),
      components: components
    }
    onOptionsChange(payload)
  }

  useEffect(() => {
    updateOptions(selectedFilterType);
  }, [selectionOptionsValue]);

  useEffect(() => {
    updateOptions(selectedFilterType);
  }, [selectedFilterType]);


  return (
    <div className="universe-support-bundle-step-two">
      <p className="subtitle-text">
        Support bundles contain the diagnostic information. This can include log files, config
        files, metadata and etc. You can analyze this information locally on your machine or send
        the bundle to Yugabyte Support team.
      </p>
      <div className="filters">
        <DropdownButton
          title={
            <span className="dropdown-text"><i className="fa fa-calendar" /> {filterTypes.find((type) => type.value === selectedFilterType).label}</span>
          }
          pullRight
        >
          {filterTypes.map((filterType, index) => {
            if(filterType.type === 'divider') {
              return <MenuItem divider />
            }
            return (
            <MenuItem
              onClick={() => {
                setSelectedFilterType(filterType.value);
                updateOptions(filterType.value, selectionOptionsValue)
              }}
              value={filterType.value}
            >
              {filterType.label}
            </MenuItem>
            );
          })}
        </DropdownButton>
      </div>
      <div className="selection-area">
        <span className="title">
          Select what you want to include in the support bundle
        </span>
        {
          selectionOptions.map((selectionOption, index) => (
            <div className="selection-option">
              <YBCheckBox
                key={`${selectionOptionsValue[index]}${index}selectionOption`}
                onClick={() => {
                  if(index === 0) {
                    for(let internalIndex = 1; internalIndex < selectionOptions.length; internalIndex++) {
                      selectionOptionsValue[internalIndex] = !selectionOptionsValue[index];
                      refs.current[internalIndex].checked = !selectionOptionsValue[index];
                    }
                    selectionOptionsValue[index] = !selectionOptionsValue[index];
                    refs.current[index].checked = selectionOptionsValue[index];
                  } else {
                    selectionOptionsValue[index] = !selectionOptionsValue[index];
                    refs.current[index].checked = selectionOptionsValue[index];
                  }
                  setSelectionOptionsValue([...selectionOptionsValue]);
                }}
                checkState={selectionOptionsValue[index]}
                input={{ref: (ref) => refs.current[index] = ref }}
                label={selectionOption.label}
              />
            </div>
          ))
        }
      </div>
    </div>
  )
}
