module.exports = [
  {
    type: 'heading',
    defaultValue: 'Eight Pebbles'
  },
  {
    type: 'text',
    defaultValue: 'Control your bed temperature from your wrist.'
  },
  {
    type: 'section',
    items: [
      {
        type: 'heading',
        defaultValue: 'Account'
      },
      {
        type: 'input',
        messageKey: 'Email',
        label: 'Email',
        attributes: {
          placeholder: 'you@example.com',
          type: 'email'
        }
      },
      {
        type: 'input',
        messageKey: 'Password',
        label: 'Password',
        attributes: {
          type: 'password'
        }
      },
      {
        type: 'text',
        defaultValue: 'Your credentials stay on your phone and are only sent to Eight Sleep to log in. Type "demo" as email to try the app without an account.'
      }
    ]
  },
  {
    type: 'section',
    items: [
      {
        type: 'heading',
        defaultValue: 'Preferences'
      },
      {
        type: 'select',
        messageKey: 'SideOverride',
        defaultValue: 'auto',
        label: 'Bed side',
        options: [
          { label: "My account's side", value: 'auto' },
          { label: 'Left', value: 'left' },
          { label: 'Right', value: 'right' }
        ]
      },
      {
        type: 'select',
        messageKey: 'Unit',
        defaultValue: 'c',
        label: 'Temperature unit',
        options: [
          { label: 'Celsius', value: 'c' },
          { label: 'Fahrenheit', value: 'f' }
        ]
      },
      {
        type: 'select',
        messageKey: 'Scale',
        defaultValue: 'level',
        label: 'Adjust in',
        options: [
          { label: 'Eight Sleep levels (-10 to +10)', value: 'level' },
          { label: 'Degrees (°C/°F)', value: 'deg' }
        ]
      },
      {
        type: 'toggle',
        messageKey: 'Haptics',
        defaultValue: true,
        label: 'Vibrate on confirm'
      },
      {
        type: 'select',
        messageKey: 'SnoozeMin',
        defaultValue: '10',
        label: 'Alarm snooze',
        options: [
          { label: '5 minutes', value: '5' },
          { label: '10 minutes', value: '10' },
          { label: '15 minutes', value: '15' }
        ]
      }
    ]
  },
  {
    type: 'submit',
    defaultValue: 'Save'
  }
];
